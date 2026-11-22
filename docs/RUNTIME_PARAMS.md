# RUNTIME_PARAMS — where the missile/weapon parameters live in `acz.exe` memory

Author: `runtime-params-re` (autonomous RE pass).
Scope: **runtime** layout of the `.wpb` weapon parameters, and how a locator can find those
objects at runtime without hard-coded heap addresses.

**Validity statement.** Every RVA here is for `ACZero\acz.exe`, **4,046,008 B**, SHA-256
`3802B0DC05C43DDA06BEA6D4124DCE611BD2956C7E9F27F9EB322615D42D5E5A`, and for the unpacked
image `ACZPropNav/work/acz_copy.exe.unpacked.exe` (3,811,952 B) produced by Steamless
(`docs/RE.md` §3.5.1). Both share section RVAs, so `file RVA == live RVA` and
`live address = exeBase + RVA` (`exeBase` is the `HMODULE` from `AczAddonInit`, in the live
run `0x7FF6B5490000` per `ACZero\ACZPropNav.log:9`).

Confidence markers: **VERIFIED** = read out of the binary/live log with the commands shown;
**INFERRED** = fits the evidence, not directly observed; **UNKNOWN** = not established.
Nothing is stated as fact that was not read.

---

## 0. TL;DR — the four answers

| Question | Answer | Confidence |
| --- | --- | --- |
| What class holds a `.wpb` record at runtime? | `WeaponModel`, vftable RVA **`0x2AE790`** (@ `.rdata`), one record type per class; records are *variable-length objects* (`0x48`-byte header + payload at `+0x48`) | **VERIFIED** (class, vtable, size, payload split) |
| Where are `lead / scope / range / startAng / speed` inside it? | **In the payload at object `+0x48` and beyond — NOT at the `.wpb` offsets, and not as a flat float array.** They are re-encoded as a sequence of *typed member sub-objects* with per-member headers, so the exact byte offsets are still **UNKNOWN** (§4.2, §10) | **VERIFIED negative** for the flat layout; offsets **UNKNOWN** |
| Why did the on-disk-bytes scan fail? | Because the game never keeps the 96-byte record (or its 80-byte payload) contiguous: it is parsed through a typed-descriptor serializer into the `+0x48` payload, one member object at a time | **INFERRED** (strong, §2–§3) |
| Can the mod patch live missile `lead/scope/range` today? | **No** (no verified write target for those four). **But yes to something better**: the live missile *instances* are exactly locatable two ways — `WeaponEx*Ctrl` vtable scan → `+0x5C` mask → `+0x80 + i*0x68` (§5), or `WeaponModel − 0x38 + 0x1F540` → ctrl pointer array (§6 R5) — which is what a true-PN hook needs | **VERIFIED** container / **UNKNOWN** param offsets |

---

## 1. The one class that models a `.wpb` record: `WeaponModel`

### 1.1 Identity — VERIFIED

```
RTTI  .?AVWeaponModel@@          vftable RVA 0x002AE790   COL RVA 0x002DF4F0
      COL signature 1, pTypeDescriptor RVA 0x00324898, name at TD+0x10 = ".?AVWeaponModel@@"
      numBaseClasses = 1 (only itself, i.e. no RTTI-visible base)
```

`dump_vtable.py` shows **exactly one virtual slot** — the scalar-deleting destructor:

```
== .?AVWeaponModel@@   vftable=0x002AE790
   [ 0] rva=0x102550  size=0x2B          ; dtor
   [ 1] 0x2DFAF8  -> this is not a slot, it is the COL pointer of the NEXT vftable
```

`WeaponModel` has **no** `GetX()`/`SetX()` virtuals (contrast `WeaponEx*Ctrl`, §5). All of its
accessors are statically-bound member functions, which is why a vtable dump shows nothing
useful about the fields. The **vftable pointer itself is the type key** used by the game's
parameter (de)serializer (§3), so `first qword == exeBase + 0x2AE790` is a *complete and
sufficient* class test at runtime.

### 1.2 `WeaponModel` is a variable-length object (`0x48` + payload) — VERIFIED

```asm
; WeaponModel::`scalar deleting destructor'   RVA 0x102550  (32 B)
0x102550  40 53                    pushq %rbx
0x102552  48 83 EC 20              subq  $0x20,%rsp
0x102556  48 8D 05 33 C2 1A 00     leaq  0x1AC233(%rip),%rax        ; = exe+0x2AE790 (own vftable)
0x10255D  48 8B D9                 movq  %rcx,%rbx                   ; rcx = this
0x102560  48 89 01                 movq  %rax,(%rcx)                 ; this->vftable = &WeaponModel::vftable
0x102563  F6 C2 01                 testb $1,%dl                      ; dl = "free?" flag
0x102566  74 0A                    je    0x102572
0x102568  BA 48 00 00 00           movl  $0x48,%edx                  ; <-- static/fixed part = 0x48
0x10256D  E8 2E C8 12 00           callq exe+0x22EDA0                ; operator delete(this, 0x48)
0x102572  48 8B C3                 movq  %rbx,%rax
0x102575  48 83 C4 20              addq  $0x20,%rsp
0x102579  5B                       popq  %rbx
0x10257A  C3                       retq
```

`0x102550` is **not unique** in `.text` (3 identical-shaped deleting dtors of size `0x48` exist),
so use it only as a *shape* check, never as a locator (§8).

Right behind it sits a **generic refcount helper that must not be attributed to `WeaponModel`** —
it is a useful example of why adjacency in `.text` is not evidence:

```asm
; RVA 0x102580 (32 B, signature UNIQUE) — a refcounted *handle* release, class UNKNOWN
0x102580  0F B7 41 08              movzwl 0x8(%rcx),%eax        ; u16 refcount-like @ +0x08
0x102584  0F B6 51 10              movzbl 0x10(%rcx),%edx       ; u8  flags-like    @ +0x10
0x102588  66 85 C0                 testw  %ax,%ax
0x10258B  74 0E                    je     +0x0E                   ; 0 -> destroy path
0x10258D  66 FF C8                 decw   %ax                     ; --counter
0x102590  80 E2 FE                 andb   $-0x2,%dl               ; clear flag bit0
0x102593  66 89 41 08              movw   %ax,0x8(%rcx)
0x102597  88 51 10                 movb   %dl,0x10(%rcx)
0x10259A  C3                       retq
; counter==0 && (flags&1): recompute the counter from a *descriptor*:
0x10259B  F6 C2 01                 testb  $1,%dl
0x10259E  74 2B                    je     ret
0x1025A0  48 8B 01                 movq   (%rcx),%rax             ; rax = object's first qword
0x1025A3  80 E2 FE                 andb   $-0x2,%dl
0x1025A6  C5 F0 57 C9              vxorps %xmm1,%xmm1,%xmm1
0x1025AA  C5 FA 10 40 24           vmovss 0x24(%rax),%xmm0        ; f32 at [obj]+0x24
0x1025AF  0F B6 41 11              movzbl 0x11(%rcx),%eax         ; u8  @ +0x11
0x1025B3  C5 FA 59 51 0C           vmulss 0xC(%rcx),%xmm0,%xmm2   ; f32 @ +0x0C
0x1025B8  C5 F2 2A C8              vcvtsi2ss %eax,%xmm1,%xmm1
0x1025BC  C5 EA 59 C1              vmulss %xmm1,%xmm2,%xmm0
0x1025C0  C5 FA 2C C0              vcvttss2si %xmm0,%eax
0x1025C4  66 89 41 08              movw   %ax,0x8(%rcx)
0x1025C8  88 51 10                 movb   %dl,0x10(%rcx)
```

**Why this is NOT `WeaponModel`.** If `(%rcx)` were a vftable, `vmovss 0x24(%rax)` would read
`RVA 0x2AE7B4`, which the file shows is the **middle of a function pointer**
(`0x14006A4B0` → bytes `b0 a4 06 40`, so `0x2AE7B4` = `0x00000001`, i.e. `1.4e-45f`, garbage).
For this code to make sense, `[obj]` must point to a **descriptor/param block**, whose `+0x24` is
a real float constant. So `0x102580` belongs to a different (refcounted-handle) class that merely
happens to be emitted next to `WeaponModel`'s destructor.

**Correction to the earlier draft of this document / to `docs/RE.md` conventions:** do not publish
`+0x08 = refcount, +0x10 = flags` as `WeaponModel` fields. They are **UNKNOWN** for `WeaponModel`:
only `+0x00` (vftable) and the payload boundary `+0x48` are proven. The `u16@+0x08 / u8@+0x10`
pair is a *generic* pattern in this binary and can be used as a *hint* at best.

So the fixed `0x48`-byte header is known only at its two ends:

| off | type | meaning | confidence |
| --- | --- | --- | --- |
| `+0x00` | qword | vftable pointer = **`exeBase + 0x2AE790`** (absolute VA, not an RVA — the image is relocated) | **VERIFIED** |
| `+0x08..+0x47` | ? | rest of the fixed header; contents **UNKNOWN** (64 B) | **UNKNOWN** |
| `+0x48` | — | **start of the variable-length payload** (§1.3) | **VERIFIED** |

Additional *hint* (not proof): the four templates of §1.4 are exactly `0x48` apart and only their
`+0x00` is initialised, so the header must be ≤ `0x48` and must not contain a self-pointer.

### 1.3 The payload starts at `+0x48` — VERIFIED

Three independent code paths in the loader agree that the object is
`0x48`-byte header **plus a variable tail at `+0x48`**:

```asm
; RVA 0x102A30 — "size of a WeaponModel described by a descriptor"
0x102A34  44 8B 02                 movl  (%rdx),%r8d              ; r8 = descriptor->count
0x102A37  48 8D 05 52 BD 1A 00     leaq  exe+0x2AE790,%rax        ; type key = WeaponModel
0x102A3E  48 89 44 24 20           movq  %rax,0x20(%rsp)          ; fake object {vftable, descriptor, sub[4]}
...
0x102ADA  E8 A1 FD 05 00           callq exe+0x162880             ; visitor: size of this descriptor tree
0x102ADF  83 C0 48                 addl  $0x48,%eax               ; <-- total = 0x48 + payload size
0x102AE2  48 83 C4 78              addq  $0x78,%rsp
0x102AE6  C3                       retq
```

```asm
; RVA 0x13CC10 — the same sum over 4 descriptors (args rcx/rdx/r8/r9, identical shape)
0x13CC34  48 8D 05 55 1B 17 00     leaq  exe+0x2AE790,%rax
0x13CC3D  48 89 45 B0              movq  %rax,-0x50(%rbp)         ; fake object again
;   ... 4x (count + rel32 sub-block decoding, see §3.1a) then:
0x13CCAB  E8 D0 59 02 00           callq exe+0x162880   (x4)      ; sizes
0x13CEB5  03 C7                    addl  %edi,%eax                ; sum
```

```asm
; RVA 0x0C7465 — the *registration* of a WeaponModel type; this is the allocation site
0x0C7661  48 8B D7                 movq  %rdi,%rdx                ; rdx = descriptor
0x0C7664  E8 C7 B3 03 00           callq exe+0x102A30             ; eax = 0x48 + payloadSize
0x0C7669  48 8B 0D A8 82 26 00     movq  exe+0x32F918,%rcx        ; global pool allocator (BSS static)
0x0C7670  41 B8 10 00 00 00        movl  $0x10,%r8d               ; align 16
0x0C7676  8B D0                    movl  %eax,%edx                ; size
0x0C7678  E8 E3 9D 13 00           callq exe+0x201460             ; Allocate(pool,size,align) -> rax
0x0C767D  48 8B C8                 movq  %rax,%rcx                ; rcx = new object
0x0C7680  48 8B D7                 movq  %rdi,%rdx
0x0C7683  48 8D 05 06 71 1E 00     leaq  exe+0x2AE790,%rax
0x0C768A  48 89 01                 movq  %rax,(%rcx)              ; object->vftable = WeaponModel
0x0C768D  48 89 8D 90 0F 00 00     movq  %rcx,0xF90(%rbp)         ; keep it
0x0C7694  E8 27 B1 09 00           callq exe+0x1627C0             ; ctor of the fixed part from the descriptor
0x0C7699  48 8D 51 48              leaq  0x48(%rcx),%rdx          ; <-- payload cursor = object+0x48
0x0C769D  48 8B 8D 90 0F 00 00     movq  0xF90(%rbp),%rcx
0x0C76A4  E8 17 B4 09 00           callq exe+0x162AC0             ; ctor of the payload at object+0x48
```

**Consequence for a locator:** a live `WeaponModel` is *not* a fixed-size object. Do not assume
a stride; `[obj]` is a vftable and `+0x48...` is an opaque, self-describing blob
until §10 pins it down.

### 1.4 Four `WeaponModel` *templates* are embedded in the weapon manager — VERIFIED

`RVA 0x16D780` initialises a **weapon manager** (part of the mission singleton, §1.5). It writes
the `WeaponModel` vftable at four consecutive `0x48`-strided offsets:

```asm
0x16D789  C6 41 28 3C              movb   $0x3C,0x28(%rcx)         ; manager head
0x16D78D  48 8D 05 FC 0F 14 00     leaq   exe+0x2AE790,%rax
0x16D794  4C 89 01                 movq   %r8,(%rcx)               ; r8 = 0
0x16D7AA  48 89 41 38              movq   %rax,0x38(%rcx)          ; WeaponModel #0  vftable
0x16D7AE  48 89 81 80 00 00 00     movq   %rax,0x80(%rcx)          ; #1   (+0x48)
0x16D7B5  48 89 81 C8 00 00 00     movq   %rax,0xC8(%rcx)          ; #2   (+0x48)
0x16D7BC  48 89 81 10 01 00 00     movq   %rax,0x110(%rcx)         ; #3   (+0x48)
0x16D7C3  48 8D 81 58 01 00 00     leaq   0x158(%rcx),%rax         ; 0x110 + 0x48 = 0x158  <-- exactly 4 slots
```

`0x38 + 4*0x48 = 0x158` — the next initialised region begins exactly at `+0x158`, which
confirms both the `0x48` size and that there are exactly **four** templates. This matches the
four shipped *missile-family* records (`mis`, `misf`, `misf2`, `mis_old`, see `docs/VANILLA.md`
§3) — **INFERRED** mapping, the code does not label them.

The same ctor also initialises, further into the manager, `16 × 0x58` at `+0x158` and `+0x708`,
`32 × 0x38` at `+0xCD8` and `+0x1400`, and two embedded weapon sub-objects at `+0x1B10` and
`+0x1FDA0` (both constructed by `exe+0x13F8E0`, which is the function that references the
`WeaponExQaamCtrl` vtable). Those are **not** `.wpb` records — they are the per-weapon-instance
state (§5). Only the four `+0x38/+0x80/+0xC8/+0x110` objects carry the `WeaponModel` type key.

### 1.5 Where the manager lives — VERIFIED (static offset chain)

```
exe+0x16E0F0   (mission-level singleton init; the block is 0xA8940 B = 689,216 B, heap)
  0x16E13B  movl $0xA8940,%edx          ; alloc size = 689,216 B
  0x16E140  movl $0x10,%r8d             ; align 16
  0x16E146  callq exe+0x201460          ; Allocate(pool@exe+0x32F918, 0xA8940, 16)
  0x16E14B  movq %rax,%rcx
  0x16E14E  callq exe+0x16D980          ; ctor of the big singleton
  0x16E153  movq %rax,0x8(%rbp)         ; stored in the mission object's field +0x08
```

```
big singleton (0xA8940 B, heap)
   +0x06A130   weapon manager                <- exe+0x16D780 constructs this sub-object
   +0x06A168   WeaponModel template #0       (= manager + 0x38)
   +0x06A1B0   WeaponModel template #1
   +0x06A1F8   WeaponModel template #2
   +0x06A240   WeaponModel template #3       (= manager + 0x110)
```

`exe+0x32F918` is a **static zero-initialised object in `.data`/BSS** owned by `acz.exe`
(the global 16-byte-aligned pool allocator, `Allocate` = `exe+0x201460`), so
`exeBase + 0x32F918` is a stable address for this build — but it is only an allocator, it holds
no back-pointer to the objects it served. The singleton itself is heap-allocated, so this chain
does **not** give a static address for the parameters; it does give a *hook target*
(`exe+0x16D780` is called exactly once per mission load, signature in §8).

---

## 2. What the runtime record is *not* — the flat-layout hypothesis is dead

`docs/VANILLA.md` §9 said "runtime placement is unverified … they are almost certainly preserved
(it is a plain struct)". **That assumption is wrong.** Evidence:

1. **Live log.** `ACZero\ACZPropNav.log:11,30,45,60,78,98,118,137` — a scan of **1.6 GB** of
   *every readable region* (the mod deliberately widened it from writable-only, see
   `src/core/patcher.cpp:317-333`) for the 8-byte needle `1.0f ('explPers') + 1600.0f ('scope')`
   found **1 … 16** candidates and rejected all of them. If the 80-byte payload were copied
   contiguously, that needle would hit at least once per live weapon object and the follow-up
   `data_off==0x10` / `type==2` validation would then accept it.
2. **The class cannot hold it.** `sizeof` fixed part = `0x48` = **72** B; the `.wpb` payload alone
   (`0x10..0x5F`) is **80** B. So the values cannot live inside the fixed part in any order.
3. **The loader is a typed serializer, not a `memcpy`.** §1.3 shows the size of a `WeaponModel`
   being *computed from a descriptor tree* (`0x102A30` → `0x162880`, `+0x48` added), and the
   payload built member-by-member by `0x162AC0` at `Object+0x48`. A `memcpy` of a fixed struct
   has no such code.
4. **Members are self-describing sub-objects with headers and type tags.** See §3.2.

**Conclusion (VERIFIED negative):** the game does not keep the `.wpb` record — or its payload —
verbatim in memory; the parameters are re-encoded. Any scanner built on the on-disk byte layout
(`kExplPers+kScope` needle, `data_off == 0x10`, `type @ +0x58 == 2`) is structurally guaranteed
to fail on this build and will keep failing. That code path
(`Patcher::ScanVanillaRecords`, `src/core/patcher.cpp:308-437`) should be downgraded to a
diagnostic and the locator moved to §6/§10.

---

## 3. How the parameters get in: the typed-descriptor ("Param") subsystem — INFERRED, strong

All of the following were read out of the binary; the *interpretation* (one generic reflection /
serialization subsystem that loads `.wpb`/`datapack` data into type-keyed objects) is INFERRED
because no single function can be proven end-to-end without a live trace.

### 3.1 Two shared idioms

**(a) descriptor blocks = `{ i32 count; i32 rel32[count] }`** — a rel32 is added to the block's
own address to get a pointer; `0` means null. Seen verbatim in `0x102A30`, `0x13CC10`, `0x0AC040`:

```asm
0x102A34  44 8B 02                 movl   (%rdx),%r8d        ; count
0x102A4A  45 85 C0                 testl  %r8d,%r8d
0x102A60  48 63 4A 04              movslq 0x4(%rdx),%rcx     ; rel32 #0
0x102A64  85 C9                    testl  %ecx,%ecx
0x102A6D  48 03 CA                 addq   %rdx,%rcx          ; ptr = block + rel32
0x102A75  41 83 F8 01              cmpl   $1,%r8d            ; count > 1 ?
0x102A87  48 63 4A 08              movslq 0x8(%rdx),%rcx     ; rel32 #1
...
```

**(b) per-member type tags** — `0x102840` decodes a type code and stores per-member metadata:

```asm
; RVA 0x102840  — "describe one value of type dl"  (signature UNIQUE, §8)
0x102840  44 0F B6 CA              movzbl %dl,%r9d
0x102844  4C 8D 15 B5 D7 EF FF     leaq   -0x10284B(%rip),%r10   ; r10 = IMAGE BASE (RVA 0)
0x10284E  88 51 0D                 movb   %dl,0xD(%rcx)           ; member.type   @ +0x0D
0x102851  41 8D 41 FF              leal   -0x1(%r9),%eax
0x102855  83 F8 14                 cmpl   $0x14,%eax              ; 20 entries
0x10285C  41 0F B6 84 02 5C 29 10 00  movzbl 0x10295C(%r10,%rax),%eax   ; jump-index table
0x102865  41 8B 94 82 50 29 10 00     movl   0x102950(%r10,%rax,4),%edx  ; RVA jump table
0x102870  FF E2                    jmpq   *%rdx
0x102872  C6 41 0E 04              movb   $0x4,0xE(%rcx)          ; member.kind   @ +0x0E  (1/2/4)
0x1028A1  41 C6 40 0F 00           movb   $0x0,0xF(%r8)           ; member.kind2  @ +0x0F  (0/1/4)
0x1028DA  41 80 60 0C FE           andb   $-0x2,0xC(%r8)          ; member.flags  @ +0x0C
0x1028E6  43 0F B6 84 0A C8 29 10 00  movzbl 0x1029C8(%r10,%r9),%eax   ; 22-entry size-class table
```

So a **member** carries a 4-byte metadata header at `+0x0C..+0x0F`
(`{u8 flags; u8 type; u8 kind; u8 kind2}`) and 22 type codes are supported.
The per-type tables are at RVA `0x102950/0x10295C` (20 entries), `0x102974/0x102980`,
`0x102998/0x1029A4`, `0x1029BC/0x1029C8` — **this is the schema decoder for every param value in
the game, `.wpb` included.**

**(c) member construction/size helpers** (all reached from the `WeaponModel` loader, §1.3):

| RVA | size | what it does | evidence |
| --- | --- | --- | --- |
| `0x1627C0` | — | attach the descriptor to the fixed part | called as `(obj, descriptor)` at `0xC7694` |
| `0x162AC0` | — | construct the payload at `obj+0x48` | called as `(obj, obj+0x48)` at `0xC76A4` |
| `0x162880` | `0x207` | "size of this descriptor tree"; reads counts at `+0x10/+0x28/+0x30/+0x38` of a descriptor and computes an aligned size | called by `0x102A30`, `0x13CC10` |
| `0x102840` | — | per-member type decode (§3.1b) | see above |
| `0x0AC262` | `0xFA` | walk 3 payload members: for each, `movq <container-vtable>,(dst)`, `0x1627C0`, `0x162AC0(dst,dst+0x48)`, `0x162880`, then **`dst += size`** | see below |
| `0x0AC35C` | `0x5C` | same for one more member (cursor already advanced) | idem |
| `0x201460` | `0x25F` | **pool allocator**: `Allocate(rcx=pool, edx=size, r8d=align) -> rax`; pool state at `+0x10/+0x18/+0x28` | `0x0C7678`, `0x16E146` |

### 3.2 Members are laid out by *their own* size, not by the `.wpb` field size — VERIFIED mechanism

From `0x0AC262` (the tail of the same loader family), the cursor advances by each member's own
serialized size — the observed granularities are **`0x18`, `0x20`, `0x38`, `0x58`**:

```asm
0x0AC262  4C 89 64 24 50        movq   %r12,0x50(%rsp)
0x0AC267  48 89 28              movq   %rbp,(%rax)             ; member header qword 0
0x0AC26A  48 8D 78 18           leaq   0x18(%rax),%rdi         ; next member cursor = +0x18
0x0AC276  C7 40 08 00 00 80 3F  movl   $0x3F800000,0x8(%rax)   ; f32 1.0f at member+0x08 !!
0x0AC27D  C7 40 0C 00 00 00 01  movl   $0x01000000,0xC(%rax)   ; {flags,type,kind,kind2} @ +0x0C  (§3.1b)
...
0x0AC294  4C 89 2F              movq   %r13,(%rdi)             ; next member: own vtable/type key
0x0AC297  48 8B CF              movq   %rdi,%rcx
0x0AC29D  E8 1E 65 0B 00        callq  0x1627C0
0x0AC2A5  48 8D 57 48           leaq   0x48(%rdi),%rdx
0x0AC2A9  E8 12 68 0B 00        callq  0x162AC0
0x0AC2B1  E8 CA 65 0B 00        callq  0x162880                ; -> eax = size
0x0AC2DD  48 63 C8              movslq %eax,%rcx
0x0AC2E0  48 03 F9              addq   %rcx,%rdi               ; cursor += size
```

**This is the decisive detail for the scanner:** a member is at minimum
`{qword header@+0, f32 value@+0x08, {flags,type,kind,kind2}@+0x0C}` = `0x10` used bytes inside a
`0x18`-aligned slot, and sub-object members are `0x48 + …` further. Therefore consecutive `.wpb`
fields are **at least `0x18` apart**, often `0x38`/`0x58`/`0x48+…` apart, and each is preceded by
a header. Two adjacent `f32`s from the file (e.g. `explPers=1.0` then `scope=1600`) can never
appear adjacent in memory — exactly what the live log shows.

> Note the `movl $0x3F800000,0x8(%rax)` above: **1.0f is used as a *default value*** for such a
> member. Combined with `1600.0f` somewhere else, this is also why "1.0 followed by 1600.0"
> threw a handful of coincidental hits.

---

## 4. Field offsets inside the runtime record — status

### 4.1 What is proven

* `WeaponModel::vftable` RVA `0x2AE790`; object fixed part `0x48`; variable payload at `+0x48`.
* The rest of the fixed header (`+0x08..+0x47`) is **UNKNOWN** — in particular it is **not** the
  `refcount`/`flags` pair of §1.2, which belongs to a different (neighbouring) class.
* The payload is a sequence of typed members, each `{qword hdr; f32 @+0x08; meta @+0x0C}` in a
  `0x18`-granular slot, sub-object members being `0x48`-headed themselves.
* The payload size is *data-dependent* (`0x48 + Σ`) — so different weapon models may have
  different payload sizes, and the four embedded templates (§1.4) appear to have a **zero-size
  payload** (they are exactly `0x48` apart) — **INFERRED** from the stride, not from a size field.

### 4.2 What is NOT proven, and why that is the honest answer

**UNKNOWN: the byte offsets of `lead`, `scope`, `range`, `startAng`, `speed` inside a live
object.** Reasons, all verified:

* The class has no virtual accessors (§1.1) — nothing to name-match.
* Every consumer of a `WeaponModel` reaches it through generic descriptor code that addresses
  members by *cursor*, not by constant offsets (§3.2), so there is no `mov [obj+0x??], xmm`
  in `.text` we can read the offset out of.
* The `.wpb` file order is not the payload order: the serializer walks a descriptor tree whose
  member count and types come with the data (`0x102A30` reads `descriptor->count`), so the layout
  is *not* fixed by the exe alone.
* The two most promising static anchors — `CIndicatorHudWeaponRange` (vftable `0x2ADD18`) and
  `CIndicatorLockonInfo` (vftable `0x2ADDD0`) — are HUD adapters that read values through
  virtuals of *other* classes; their small slots (`0x0DBEB0`, `0xDCDD0`, `0x0DC100`) contain no
  float read from a `WeaponModel` (checked: no `WeaponModel` vtable xref anywhere near them).

**Do not guess these offsets and do not ship them behind `ForceOffsets`.** The payoff of one
`Probe=1` run (§6 R3 + §10) is far higher than any static guess.

### 4.3 Units and value semantics — also UNKNOWN at runtime

On disk, `docs/VANILLA.md` §2 is still the reference (scope/range m, `lead` = frames-ish gain,
`startAng` deg, `speed` m/s, `startAng` a `u8` in the file). **At runtime, units are unproven**
and there are concrete signs of conversion rather than copying:

* The engine works in radians — `vcomiss 3.1415927` / `vsubss 6.2831855` wrap constants in the
  missile update (`docs/RE.md` §3.7.1), and the seeker functions multiply by `0.0174533`
  (deg→rad, see `work/weapon_const_xrefs.txt`: 11 refs incl. `0x9C9F`, `0x98320`, `0x10F9BB`).
* `startAng` is a `u8` on disk; at runtime angle values are `f32` radians in every structure we
  read. If the runtime keeps it as a `u8` it must be *derived* (`0x1AC6D` computes bytes as
  `int(scale * f32)` — see §5.3), and if it keeps it as a float it is probably radians.
* `lead` may stay `40.0f` ("frames") or be pre-multiplied by a frame time. A `40.0f` is one of
  the most common floats in the process (the live log shows thousands of anchors), so it is a
  bad anchor and a bad unit to assume either way.

So: **`lead`/`startAng` unit conversion is an open question that the probe must answer with
numbers, not with the on-disk convention.**

---

## 5. What *is* exactly locatable today — the live missile container (`WeaponEx*Ctrl`)

This is the part of the report the mod can act on immediately. It is fully verified by two
independent code paths plus a cross-check on the class's own virtuals.

### 5.1 Class and vtable — VERIFIED (`tools/re/dump_vtable.py`)

| class | vftable RVA |
| --- | --- |
| `WeaponExFireCtrl` (generic/non-homing) | `0x2AF470` |
| `WeaponExQaamCtrl` | `0x2AF740` |
| `WeaponExSaamCtrl` | `0x2AFAE8` |
| `WeaponExXlaaCtrl` | `0x2AF560` |
| `WeaponExXmaaCtrl` | `0x2AF308` |
| `WeaponExXagmCtrl` | `0x2AF980` |
| `WeaponExLasmCtrl` / `Lagm` / `Bomb` / … | `0x2AFB60` / `0x2AF5D8` / `0x2AFC50` (40 classes total, `work/acz_rtti.tsv`) |
| `WeaponModel` (data record, §1) | `0x2AE790` |

All of these live in `.rdata` (RVA range `0x27D000..0x30B4C0`), i.e. **read-only image memory**, and
the pointer stored in each object is the relocated **absolute VA** `exeBase + RVA`
(verified: the file holds `0x00000001402AE790` at `0x2AE788`-adjacent slots before relocation).
So the runtime test for an object of class *C* is literally
`*(uintptr_t*)obj == exeBase + vtableRva`.

### 5.2 Object field map — VERIFIED

Cross-checked from three unrelated functions (disassembly in §5.2–§5.3):

| off | type | meaning | evidence |
| --- | --- | --- | --- |
| `+0x00` | qword | vftable (absolute VA) | all slots |
| `+0x08` | — | `WeaponRef` sub-object (RTTI base `.?AVWeaponRef@@`, `mdisp=8`) | `docs/RE.md` §3.7.1 RTTI chain |
| `+0x10` | qword | pointer to the owner/asset object (used by the per-missile update: `movq 0x10(%rcx),%rax; movq (%rax),%r11; movzbl 0x34(%rax)`) | `0x91E45` |
| `+0x4C` | i32 | HUD value #1 (ammo-like) | `0x1026F2` |
| `+0x50` | i32 | HUD value #2 | `0x1026F7` |
| `+0x54` | i32 | HUD value #3 | `0x1026FD` |
| `+0x58` | f32 | HUD value #4 (progress/timer-like) | `0x102703` |
| **`+0x5C`** | **u32** | **live-missile bitmask, bits 0..15** | `0x9DBB0`, `0xA3FC0`, `0x1026B9` |
| **`+0x60`** | **i32** | **"current/most recent slot index"**, `-1` when idle | `0xA3FC2` (`= -1`), `0x1026B6`, `0x102602` |
| `+0x64` | u16 | HUD hue/tint (converted to ARGB by vtable slot 1) | `0x903A0`, `0xA3FCC` |
| `+0x7C` | u16 | `0xFFFF` sentinel | `0xA3FD0` |
| **`+0x80`** | **array** | **16 × `0x68`-byte missile slots** | `0x9DBB0`: `leaq 0x80(%rcx),%rsi … addq $0x68,%rsi … cmpl $0x10,%edi` |
| `+0x700` | — | end of the slot array | `0x80 + 16*0x68` |

The two independent confirmations of the `+0x5C`/`+0x60` pair:

```asm
; (1) exe+0x09DBB0 — slot 9 of every homing weapon, the per-frame salvo dispatcher
0x09DBCA  C5 FA 10 35 26 6A 23 00  vmovss exe+0x2D45F8,%xmm6        ; mode constant passed to C1
0x09DBD2  48 8B E9              movq   %rcx,%rbp                  ; this
0x09DBD5  48 8D B1 80 00 00 00  leaq   0x80(%rcx),%rsi            ; slot[0]
0x09DBDC  33 FF                 xorl   %edi,%edi                  ; i = 0
0x09DBDE  BB 01 00 00 00        movl   $1,%ebx                    ; bit = 1
0x09DBE3  85 5D 5C              testl  %ebx,0x5C(%rbp)            ; mask & (1<<i)
0x09DBE6  74 1B                 je     skip
0x09DBED  4C 8B C6              movq   %rsi,%r8                   ; arg3 = slot
0x09DBF4  8B D7                 movl   %edi,%edx                  ; arg2 = i
0x09DBFB  48 8B CD              movq   %rbp,%rcx                  ; arg1 = this
0x09DBFE  E8 BD 41 FF FF        callq  exe+0x091DC0                 ; per-missile update
0x09DC03  FF C7                 incl   %edi
0x09DC05  D1 C3                 roll   %ebx
0x09DC07  48 83 C6 68           addq   $0x68,%rsi
0x09DC0B  83 FF 10              cmpl   $0x10,%edi
0x09DC0E  7C D3                 jl     loop
```

```asm
; (2) exe+0x1025D0 — "HUD info for weapon slot idx";  RCX here is the *weapon manager*
;     (the object built by exe+0x16D780), NOT a ctrl — that is why the offsets are 0x1F5xx:
0x1025FB  44 8B 89 F0 06 00 00  movl   0x6F0(%rcx),%r9d           ; manager: current idx (=-1 idle)
0x102602  8B 81 E8 06 00 00     movl   0x6E8(%rcx),%eax           ; manager: mask
0x102608  44 0F A3 C8           btl    %r9d,%eax                  ; mask & (1<<idx)
...
0x1026A2  0F B6 81 30 F5 01 00  movzbl 0x1F530(%rcx),%eax          ; manager: "which EX slot is equipped"
0x1026A9  48 8B BC C1 40 F5 01 00  movq 0x1F540(%rcx,%rax,8),%rdi  ; rdi = WeaponEx*Ctrl*
0x1026B6  8B 4F 60              movl   0x60(%rdi),%ecx            ; ctrl->+0x60  (idx)
0x1026B9  8B 47 5C              movl   0x5C(%rdi),%eax            ; ctrl->+0x5C  (mask)
0x1026BC  0F A3 C8              btl    %ecx,%eax                  ; live?
0x1026BF  72 25                 jb     live
0x1026E6  48 8B 07              movq   (%rdi),%rax
0x1026E9  48 8B CF              movq   %rdi,%rcx
0x1026EC  FF 50 08              callq  *0x8(%rax)                  ; vtable slot 1 = per-class "GetHudColor"
0x1026EF  41 89 06              movl   %eax,(%r14)                ; -> packed ARGB
0x1026F2  8B 47 4C              movl   0x4C(%rdi),%eax ; ... 0x50, 0x54, vmovss 0x58
```

Identification of `rdi` as a `WeaponEx*Ctrl` is **VERIFIED by three coincidences**: (a) vtable
slot 1 is the per-class ARGB packer below, (b) `+0x5C`/`+0x60` are the same mask/index pair that
`0x9DBB0` walks, (c) `0xA3FC0` (slot 0 of the same vtables) initialises exactly those fields.
That the manager holds them as a pointer array at `+0x1F540` indexed by `u8 +0x1F530` is
**INFERRED** (only this one function reads it).

and the per-class slot 1 really is an ARGB packer that reads `+0x64`:

```asm
; WeaponExQaamCtrl vtable slot 1 = RVA 0x0903A0
0x0903A0  0F B7 51 64           movzwl 0x64(%rcx),%edx
0x0903A4  69 C2 00 BE 00 00     imull $0xBE00,%edx,%eax      ; 190
0x0903AA  69 CA 00 00 78 00     imull $0x780000,%edx,%ecx    ; 120
0x0903B0  25 00 00 FF 00        andl  $0xFF0000,%eax
0x0903B5  81 E1 FF 00 00 FF     andl  $0xFF0000FF,%ecx
0x0903BB  0B C1                 orl   %ecx,%eax
0x0903BD  69 CA FA 00 00 00     imull $0xFA,%edx,%ecx        ; 250
0x0903C3  81 E1 00 FF 00 00     andl  $0xFF00,%ecx
0x0903C9  0B C1                 orl   %ecx,%eax
0x0903CB  C1 E8 08              shrl  $0x8,%eax
0x0903CE  0F BA E8 1F           btsl  $0x1F,%eax              ; opaque
0x0903D2  C3                    retq
```

### 5.3 The missile slot itself (`ctrl + 0x80 + i*0x68`) — VERIFIED field map

Taken from every slot-relative access in the per-missile update `exe+0x091DC0`
(the `%r8`/`%r15` aliases are the slot pointer handed over by `0x9DBB0`):

| off | type | meaning | instruction |
| --- | --- | --- | --- |
| `+0x00` | f32×3 (+w) | **position vector** (read as `vmovups (%r8)` and used with `vsubss (%r15)`, `+0x4`, `+0x8`) | `0x91DE9`, `0x9202D`, `0x9204E`, `0x92059` |
| `+0x10` | f32 | **heading scalar #1** (radians) | written `0x9221B`, `0x92254` |
| `+0x14` | f32 | **heading scalar #2** (radians) | written `0x92240` |
| `+0x18` | qword | pointer written once per update (`movq %rdi,0x18(%r15)`) — **INFERRED**: target/scene-object link | `0x92246` |
| `+0x20` | f32×4 | **second vector** (read `vmovups 0x20(%r15)`, individual lanes at `+0x20/+0x24/+0x28/+0x2C`) and rewritten every frame; a dot-product length `sqrt(Σ²)` is computed from the 4 lanes at `0x91E80..0x91EC8` | `0x91DFF..0x91E28`, `0x920D1` |
| `+0x50` | u8 | **alive/state byte** (`movzbl 0x50(%r8)` in the update; cleared/set by the update) | `0x91DEE`, `0x91F8D`, `0x92269` |
| `+0x51` | u8 | secondary state byte | `0x91F97`, `0x92275` |
| `+0x52` | u16 | set to `0xFFFF` (`movw $0xffff,0x52(%r15)`) — "no target"/sentinel | `0x9225A` |
| `+0x54..+0x67` | ? | not touched by `C1`; **UNKNOWN** (20 B) | — |

The four `0x48`/`0x0x`-style fields that `docs/CALIBRATION.md:174` proposes (`slot+0x00` pos,
`slot+0x10/0x14` heading, `slot+0x50` alive) are **confirmed** by the table above; everything
beyond `+0x54` is not part of the guidance state that `C1` reads.

There is a **second, closely related struct** written by `exe+0x01AC6D` with the same shape but
**more** bytes (`+0x20..+0x40` floats, `+0x44/+0x48/+0x4C/+0x50` ints, `+0x54 u16`,
`+0x56 u16`, `+0x58 u16`, `+0x5A u8`, `+0x5B u8`, `+0x5C..+0x5E u8`, `+0x60 i32`,
`+0x64 u16`, `+0x66..+0x6B u8`) — because it writes past `0x67` it is **not** the `0x68`-stride
missile slot; it is a *launch/spawn parameter* struct (`INFERRED`). It is interesting because two
of its bytes are computed as `int(scale * f32)` from the firing aircraft
(`vcvttss2si` at `0x1AC7D/0x1AC89` → `movb %r10b,0x5A(%rbx)`, `movb %r11b,0x5B(%rbx)`), i.e.
**angles are quantised to bytes somewhere in the weapon pipeline** — a hint that the parameter
flow deg→byte exists, but not proof of where `startAng` lives. Recorded so nobody chases it twice.

---

## 6. Runtime locators, in order of value

Everything below is implementable in the shipping `Probe=1` path; none needs a debugger.

### R1 — `WeaponModel` instances by vtable (already coded, needs to be *widened*)

`Patcher::ProbeRttiClasses()` (`src/core/patcher.cpp:494-594`) already does the right thing:

```text
vtable = exeBase + 0x2AE790
for every writable region:
    for every 8-byte-aligned qword == vtable:  -> candidate object
    dump floats at obj+0x00 .. obj+0x100
```

Three corrections, all from this report:

1. **Scan readable, not only writable**, regions — the four templates of §1.4 sit inside a heap
   block (fine), but the templates are initialised by a zeroing allocator path and a future
   build could place them in a read-only mapping. `ScanVanillaRecords` already scans all readable
   regions (`patcher.cpp:317-333`); make `ProbeRttiClasses` do the same.
2. **Walk past `+0x100`.** The payload starts at `+0x48` and its members are `0x18/0x38/0x58`
   slots; the interesting floats can be well past `+0x100`. Dump at least `obj+0x48 .. obj+0x300`,
   and additionally dump `obj+0x48 …` of the **first member of each `0x18` slot** (see R3).
3. **Log the raw qwords too**, not only floats: the payload begins with member header qwords and
   type tags at `member+0x0C..+0x0F` (§3.2). A hex dump beats a float dump for identifying them.

Validation rules for a hit (all cheap, all read-only):

```text
+0x00 == exeBase+0x2AE790                         ; class test (sufficient)
+0x08..+0x47: no assumption                       ; fixed header contents UNKNOWN (see 1.2)
+0x48..      : payload, log it raw                 ; do not interpret yet
object lies in a region whose State != MEM_FREE   ; (implicit in the scan)
```

The **only** reliable class test is the vftable qword. Do not add refcount/flag plausibility
checks: the fixed header's semantics are UNKNOWN (§1.2) and the four embedded templates
(§1.4) get nothing but their vftable written.

### R2 — capture the allocation instead of the object (gives the payload size *and* the caller)

`exe+0x201460` is the pool allocator (`Allocate(pool, size, align)`); the `WeaponModel`
allocation site is `0x0C7678` and passes `size = 0x48 + payloadSize` from `exe+0x102A30`.

Probe variant: hook `exe+0x201460` (or just log from a trampoline), and for every call whose
**return address** is `exe+0x0C767D` (registration) — or, more generally, whose `size` is in
`[0x48, 0x800]` — log `{size, returned ptr}`. Then, one frame later, read `*(qword*)ptr` and
keep it if it equals `exeBase+0x2AE790`.

Why this is worth the hook: it yields `payloadSize = size - 0x48` for **each** weapon model, which
immediately bounds the layout (e.g. `payloadSize / 0x18` ≈ field count) and separates
`mis`/`misf`/`misf2` if they differ. It also survives a game update better than any byte pattern.

Signature for `Allocate` (`exe+0x201460`, first 24 B): see §8.

### R3 — anchored, *stride-agnostic* co-occurrence (the cheap decisive experiment)

Do **not** search for a contiguous payload. Search for one rare value, then report the *distances*
to the other values. This directly tests the §3.2 prediction (members `≥ 0x18` apart).

```text
anchor   = 1600.0f  (scope; the live run showed ~3k anchors/region-set, manageable)
for each anchor hit A:
    for each candidate other value V in {3000.0f (range), 90/40 as f32, 1000 as u16 and f32}:
        for each hit B of V within  [A-0x2000, A+0x2000]:
            delta = B - A
            tally[(V, delta)] += 1
report the top deltas: a (1600,3000) delta that repeats across many anchors IS the layout
```

Practical details:

* Compute the deltas in **one pass over the whole region set per value** (bucket all hits first,
  then sort and diff), otherwise it is O(n²).
* `40.0f` is worthless as an anchor (thousands of coincidences, live log lines 18-20); use it only
  as a *confirming* value once `1600 ↔ 3000` has produced a delta.
* Also test the **byte/`u16` encodings**: `startAng` may be a `u8` (90) or a `u16` (90) and `speed`
  a `u16` (1000); search for those byte values too, and for `i32` 1000.
* If no delta repeats, the re-encoding is not a shift of the file order — then go to R4/§10-E3.

Existing but insufficient: `ProbeValueNeighborhoods()` (`patcher.cpp:442-492`) prints only
`±0x30` in `0x10` steps around up to 6 hits per value. With `0x18/0x38/0x58` member slots that
window is too small, `0x10` steps hide the `0x18` granularity, and 6 hits is far too few.

### R4 — hook the per-frame enumerator (needs no parameter layout at all)

`exe+0x09DBB0` (slot 9, `this`-only ABI) is entered once per frame per weapon; from `this` alone
you get `mask`, 16 slot pointers and — through each slot — the position/heading vectors of §5.3.
That is everything PN needs *except* the four static parameters, and it is exactly what
`docs/CALIBRATION.md:166-181` asks for. This is the recommended next implementation step; it does
not depend on this document's open question.

### R5 — from one `WeaponModel` to the whole weapon set (the "free" chain)

The weapon manager that embeds the four `WeaponModel` templates (§1.4) **also holds the array of
weapon-controller pointers** used by the HUD path (`exe+0x1025D0`, §5.2):

```asm
0x1026A2  0F B6 81 30 F5 01 00     movzbl 0x1F530(%rcx),%eax          ; M->selected weapon index (u8)
0x1026A9  48 8B BC C1 40 F5 01 00  movq   0x1F540(%rcx,%rax,8),%rdi  ; ctrl = *(M + 0x1F540 + idx*8)
0x1026B6  8B 4F 60                 movl   0x60(%rdi),%ecx            ; ctrl->+0x60 mask-index
0x1026B9  8B 47 5C                 movl   0x5C(%rdi),%eax            ; ctrl->+0x5C live mask
```

`M` is the same object `exe+0x16D780` constructs (it reads `+0x6E8/+0x6F0/+0x6F4/+0x1FD90`, all of
which that ctor initialises). So:

```text
M           = (address of any WeaponModel template) - 0x38      ; templates are M+0x38..M+0x110
for idx in 0..15:
    ctrl = *(uintptr_t*)(M + 0x1F540 + idx*8)                   ; MAY be null
    if ctrl is null                       -> next
    vt   = *(uintptr_t*)ctrl
    if vt not in {exeBase+0x2AF470, +0x2AF740, +0x2AFAE8, +0x2AF560, +0x2AF308, +0x2AF980, ...}
                                          -> next   ; validate against the 40 ctrl vtables
    mask = *(u32*)(ctrl+0x5C) ; for i in 0..15: if mask&(1<<i): slot = ctrl+0x80+i*0x68
```

**Confidence:** the *offsets* are VERIFIED (`0x1F530`/`0x1F540` read by `0x1026A9`, `+0x6E8/+0x6F0`
read by `0x102602`/`0x1025FB`, and `0x16D780` initialises exactly those); that the array at
`+0x1F540` is a *weapon-controller pointer array* is **INFERRED** from the single call site and
from the callee's use of `+0x5C/+0x60` (see §5.2) — **validate every dereference** and expect the
count to be ≤ 16. Also note this is a *different, more direct* route to the mod's real need than
scanning for 40 vtables, and it is the only route that works for the plain MSL (which has no
`WeaponEx*Ctrl` class of its own — `docs/RE.md` §3.7).

---

## 7. Re-locatable signatures (auto-generated, `.text`-unique in this build)

Generated with `work/rp/sig.py` (rel32/RIP displacements wildcarded; `??` = wildcard). Uniqueness
was measured against `.text` of the unpacked image — re-measure after every game update.

```text
refcounted-handle release (NOT WeaponModel) rva=0x102580  unique=1
  0F B7 41 08 0F B6 51 10 66 85 C0 74 0E 66 FF C8 80 E2 FE 66 89 41 08 88 51 10 C3 F6 C2 01 74 2B

WeaponModel size-from-descriptor        rva=0x102A30  unique=1
  48 83 EC 78 44 8B 02 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 33 C0 48 89 54 24 28 45 85 C0 7F 11 48

weapon manager ctor (templates at +0x38) rva=0x16D780  unique=1
  40 53 48 83 EC 20 45 33 C0 C6 41 28 3C 48 8D 05 ?? ?? ?? ?? 4C 89 01 4C 89 41 08 48 8B D9 4C 89

WeaponEx*Ctrl slot0 ("clear state")     rva=0x0A3FC0  unique=1
  33 C0 C7 41 60 FF FF FF FF 89 41 5C 66 89 41 64 66 C7 41 7C FF FF C3 CC CC CC CC CC CC CC CC CC

homing slot9 salvo dispatcher           rva=0x09DBB0  unique=1   (48-byte form; the 32-byte form is NOT unique)
  48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 C5 F8 29 74 24 30 C5 FA 10 35 ?? ?? ?? ?? 48 8B E9 48 8D B1 80 00 00 00 33 FF BB 01

per-missile update C1                   rva=0x091DC0  unique=1
  48 8B C4 C5 FA 11 58 20 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 A8 FE

per-member type decode                  rva=0x102840  unique=1
  44 0F B6 CA 4C 8D 15 ?? ?? ?? ?? 4C 8B C1 88 51 0D 41 8D 41 FF 83 F8 14 77 28 48 98 41 0F B6 84 02 5C 29 10 00 41 8B 94 82 50 29 10 00 49 03 D2

payload member ctor                     rva=0x0AC262  unique=1
  4C 89 64 24 50 48 89 28 48 8D 78 18 48 8B AC 24 88 00 00 00 C7 40 08 00 00 80 3F C7 40 0C 00 00 00 01 48 89 46 60 48 85 ED 74 56 48 8B 5E 50 48
```

**Not usable as a locator:** `WeaponModel`'s deleting dtor `0x102550` — its 48-byte pattern occurs
**3×** in `.text` (three classes with a `0x48`-byte instance share the shape; only the wildcarded
RIP displacement differs). Use the vftable-qword compare (R1) — there is no *unique* single-byte
pattern for this class, only the type key.

---

## 8. Reproduction — exact commands

All scratch tooling is in `ACZPropNav/work/rp/` (built for this pass, ~70 MB of cache,
regenerable, safe to delete):

```sh
PY="python"
PE="work/acz_copy.exe.unpacked.exe"       # 3,811,952 B, Steamless output
RTTI="work/acz_rtti.tsv"

# --- 0. one-time: verify the image is the expected build -------------------
$PY tools/re/pe_report.py "$PE"        # sha/sections; .text rva=0x1000 vsize=0x27B92F

# --- 1. one-time: full disassembly index (python subprocess pipes are blocked
#        by the sandbox, so objdump must write to a FILE from the shell) -----
llvm-objdump -d --start-address=0x140001000 --stop-address=0x14027C92F "$PE" > work/rp/idx/full.txt
$PY work/rp/idx.py parse work/rp/idx/full.txt work/rp/idx 0x140000000
#   -> 622,083 instructions, 8,219 .pdata functions

# --- 2. query: what references a vtable / a function ----------------------
$PY work/rp/idx.py refs work/rp/idx 0x2AE790      # WeaponModel vftable  -> 6 real sites
$PY work/rp/idx.py refs work/rp/idx 0x16D780      # weapon-manager ctor  -> 1 site
$PY work/rp/idx.py func work/rp/idx 0x102550      # disassemble one function
$PY work/rp/idx.py list work/rp/idx 0x0C7465 0x110

# --- 3. query: which functions touch a given displacement from one base ---
cd work/rp && $PY fieldscan.py idx --offs 0x54,0x58,0x5A --min 3 --disp-range 0x0,0x80 --top 30

# --- 4. RTTI / vtable dumps (existing tools) ------------------------------
$PY tools/re/dump_vtable.py "$PE" "$RTTI" --grep 'WeaponModel' --max 8
$PY tools/re/dump_vtable.py "$PE" "$RTTI" --grep 'WeaponEx(Qaam|Fire)Ctrl' --max 14
$PY tools/re/owners.py      "$PE" "$RTTI" --class WeaponModel --maxslot 24

# --- 5. signatures (re-derive after a game update) ------------------------
cd work/rp && $PY sig.py idx ../acz_copy.exe.unpacked.exe 0x102580,0x102A30,0x16D780,0xA3FC0,0x9DBB0,0x102840,0xAC262 --len 32

# --- 6. raw PE reads (section/dword/qword at an RVA) ---------------------
$PY - <<'EOF'
import struct
buf=open(r'ACZPropNav/work/acz_copy.exe.unpacked.exe','rb').read()
EOF
# (in practice use work/rp/idx.py or tools/re/*; they already do rva->file mapping)
```

Full re-derivation after a game update:

```text
1. re-unpack with Steamless  (docs/RE.md §3.5.1)          -> new acz_copy.exe.unpacked.exe
2. rtti_scan.py  -> acz_rtti.tsv                          -> find ".?AVWeaponModel@@" vftable RVA
3. dump_vtable.py --grep WeaponModel                      -> confirm 1 slot = deleting dtor of 0x48
4. idx.py build (step 1 above) + refs <vftable>           -> the 5-6 construction sites
5. idx.py func <size-from-descriptor>                     -> re-read the "+0x48" constant
6. sig.py for every RVA in §7                             -> re-check uniqueness == 1
```

---

## 9. Risks and "do not"

* **Do not** write to `WeaponModel+0x48…` on a guess. The payload is variable-length and
  self-describing; a wrong offset corrupts member headers that the loader validates.
* **Do not** treat `0x102550` as *the* `WeaponModel` dtor when scanning (3 look-alikes).
* **Do not** use `40.0f` as an anchor (thousands of coincidences).
* **Do not** assume the four embedded templates of §1.4 are the *player's* records — they are
  initialised with a vftable and nothing else; the live records are separately allocated
  (`Allocate(0x48+size,16)`, §1.3); their lifetime/ownership is **UNKNOWN**, so a cached pointer can go
  stale on a level change. Re-locate rather than cache.
* **Do not** parse `.rdata 0x2D4100..0x2D4D80` as a weapon table (compiler float pool,
  `docs/RE.md` §3.7.1) — this pass found nothing there either.
* **Do not** hook during `DllMain` (`docs/RE.md` §1.5); and `C1` (`0x91DC0`) needs
  `rcx/rdx/r8/xmm3` preserved (`docs/RE.md` §10.1).

---

## 10. The decisive next experiments (ordered, cheap)

* **E1 — `Probe=1`, 60 s in a mission with missiles fired.** The path is already wired
  (`patcher.cpp:1038-1039` → `ProbeRttiClasses`, `ProbeValueNeighborhoods`) but the shipping logs
  contain **no** `================ PROBE ================` block, i.e. it has never actually run.
  Edit `ACZero\ACZPropNav.ini` by hand to add `[runtime] Probe = 1` **and `LogLevel = 2`**
  (the probe block goes through `L().Key()`, which is silent at the default `LogLevel = 1`,
  so `Probe=1` alone produces nothing).
  Expected in `ACZPropNav.key.log`: `WeaponModel vtable … instance count`, followed by float
  grids. **This single run answers R1 (how many objects) and whether the payload contains the
  vanilla floats at all.**
* **E2 — apply the R1 widening (§6 R1 items 1-3).** Dump `obj+0x48 .. obj+0x300` and the raw
  qwords; this is what reveals the member headers of §3.2 and their type tags.
* **E3 — implement R3** (anchored deltas). If a repeating `(1600→3000)` delta appears, the layout
  is a shift and the offsets become immediately usable. If nothing repeats, the payload is fully
  re-encoded and the only sane route is R2+R4 (hook the allocator / use the slot array).
* **E4 — hook `exe+0x201460` with return-address filter `0x0C767D`** and log `size`; report
  `payloadSize = size - 0x48` per weapon model. Cross-check with E2/E3: a `payloadSize` that is
  `≈ N * 0x18` (or a sum of `0x18/0x38/0x58` slots) confirms §3.2.
* **E5 — only if all of the above fails:** put a hardware breakpoint on the *first* member of the
  payload (`obj+0x48`) after E1 located an object, and read the offsets out of the writer
  instruction (this is the x64dbg workflow of `docs/RE.md` §3.7.2, applied to a different target).

---

## 11. Answers to the five questions in the brief

1. **`WeaponModel` class shape** — vftable `0x2AE790`; exactly one virtual (deleting dtor
   `0x102550`, `0x2B`); fixed part `0x48` B whose only proven field is `+0x00` = the vftable; **the payload
   starts at `+0x48`** and is
   variable-length; no `Get/Set` virtuals. (§1)
2. **The `.wpb` parser → runtime conversion** — a typed-descriptor serializer, not a `memcpy`:
   descriptor blocks `{i32 count; i32 rel32[count]}` (`0x102A30`, `0x13CC10`, `0x0AC040`), per-member
   type decode `0x102840` with 22 type codes and tables at `0x102950/0x10295C/0x102974/0x102980/
   0x102998/0x1029A4/0x1029BC/0x1029C8`, member constructors `0x1627C0`/`0x162AC0`/`0x162880`,
   payload member builder `0x0AC262`, pool allocator `0x201460`, global pool at `exe+0x32F918`.
   Members are `0x18/0x20/0x38/0x58`-granular with their own headers. (§3)
3. **Cross-check with the missile dispatch** — done: `0x9DBB0`'s `this` is a `WeaponEx*Ctrl`
   (verified twice, §5.2/§5.3); it does **not** own a `WeaponModel`: the parameter handoff is
   through the `WeaponRef` sub-object at `+0x08` and the generic loader, and no `WeaponEx*Ctrl`
   slot reads `+0x0C`-style params from a model pointer. The slot array at `this+0x80` holds
   *live missile kinematic state* (pos/heading/state byte), **not** the static parameters. (§5)
4. **Deliverables** — container class + vtable RVA (`0x2AE790`) and the object test
   (`*(qword*)obj == exeBase+0x2AE790`); the `WeaponEx*Ctrl` family vtables and the live-missile
   recipe (`+0x5C` mask → `+0x80+i*0x68`, 16 slots) plus the `WeaponModel − 0x38 → +0x1F540`
   pointer-array shortcut (§6 R5); signatures for 8 functions (§7); the dump recipes (§6 R1/R3);
   **field offsets of the five parameters: UNKNOWN** with the reason and the experiment that will
   produce them (§10).
5. **Alternative layout (if not in a `WeaponModel`-like object)** — partially the case: the
   *guidance-relevant* live state is in the `0x68`-stride slot array, the *static* parameters are
   in the `+0x48` payload of a separately-allocated `WeaponModel`, and the four templates in the
   weapon manager are empty. There is **no** per-aircraft parameter array: `docs/VANILLA.md` §1
   already proves all aircraft share `mis/mpl/mis.wpb`, and nothing in this pass contradicts it.

---

## 12. For the Lead — concrete, minimal changes to `src/core/patcher.cpp`

1. `ScanVanillaRecords()` — keep it, but **log the candidate addresses and a hexdump** of the
   rejected `16` (the live log proves the needle hits rarely enough to be inspectable), and stop
   treating a miss as "another mod changed the data". After this pass the expected result of that
   scan is *permanent* miss on every build; the log text should say so
   ("the engine re-encodes `.wpb`; see docs/RUNTIME_PARAMS.md").
2. `ProbeRttiClasses()` — scan readable regions, dump `obj+0x00..0x300`, log raw qwords as well as
   floats, and rely on the vftable qword alone (no refcount/flag plausibility checks).
3. `ProbeValueNeighborhoods()` — replace the `±0x30 / 0x10`-step window with the R3 anchored-delta
   tally over `±0x2000`, and include `u8/u16/i32` encodings of `startAng`/`speed`.
4. Add the R2 allocator probe (`exe+0x201460`, filter return address `exe+0x0C767D`) — ~30 lines,
   read-only, gives `payloadSize` per model.
5. Add `WeaponModel` and the `WeaponEx*Ctrl` vtables to the INI-visible probe hints so a user can
   feed back a `key.log` after a game update without a rebuild.

---

## Appendix A — index of every RVA this pass established

| RVA | kind | what | confidence |
| --- | --- | --- | --- |
| `0x2AE790` | `.rdata` data | `WeaponModel` vftable (type key) | VERIFIED |
| `0x2DF4F0` | `.rdata` data | `WeaponModel` RTTI COL | VERIFIED |
| `0x102550` | `.text` | `WeaponModel` deleting dtor (frees `0x48`) — **not unique**, 3 look-alikes | VERIFIED |
| `0x102580` | `.text` | generic refcounted-handle release — **NOT** `WeaponModel` (§1.2) | VERIFIED (negative) |
| `0x102A30` | `.text` | `sizeof(WeaponModel) = 0x48 + descriptor sizes` | VERIFIED |
| `0x13CC10` | `.text` | same sum over 4 descriptors | VERIFIED |
| `0x0C7465` | `.text` | type registration; allocates a `WeaponModel` (`0xC7678`) | VERIFIED |
| `0x0AC040`, `0x0AC262`, `0x0AC35C` | `.text` | descriptor-driven member loaders/builders | VERIFIED (code), INFERRED (role) |
| `0x1627C0`, `0x162AC0`, `0x162880` | `.text` | attach descriptor / build payload / size visitor | VERIFIED (call shape) |
| `0x102840` | `.text` | per-member type-code decode, 22 types | VERIFIED |
| `0x102950…0x1029E0` | `.text` (inline data) | type tables (jump + size class), 20/22 entries | VERIFIED (addresses) |
| `0x201460` | `.text` | pool `Allocate(pool,size,align)` | VERIFIED |
| `0x32F918` | `.data`/BSS | global pool allocator object (static) | VERIFIED (address) |
| `0x16E0F0` | `.text` | mission-singleton init; allocates `0xA8940` | VERIFIED |
| `0x16D980` | `.text` | big-singleton ctor (writes `+0x108` = vtable `0x2ADA38`) | VERIFIED |
| `0x16D780` | `.text` | weapon-manager ctor; 4× `WeaponModel` at `+0x38,0x80,0xC8,0x110` | VERIFIED |
| `0x2ADA38` | `.rdata` data | vtable `.?AVCMissionControlDump@@` (written into singleton `+0x108`) | VERIFIED |
| `0x2AF470/0x2AF740/0x2AFAE8/0x2AF560/0x2AF308/0x2AF980` | `.rdata` | `WeaponEx{Fire,Qaam,Saam,Xlaa,Xmaa,Xagm}Ctrl` vftables | VERIFIED |
| `0xA3FC0` | `.text` | ctrl slot 0: clears `+0x5C=0`, `+0x60=-1`, `+0x64=0`, `+0x7C=0xFFFF` | VERIFIED |
| `0x903A0` | `.text` | ctrl slot 1: `u16 +0x64` → packed ARGB | VERIFIED |
| `0x9DBB0` | `.text` | homing slot 9: 16×(mask bit → `this+0x80+i*0x68` → `0x91DC0`) | VERIFIED |
| `0x91DC0` | `.text` | per-missile update; slot field map (§5.3) | VERIFIED |
| `0x1AC6D` | `.text` | launch/spawn param writer; quantises angles to bytes | VERIFIED (code), role INFERRED |
| `0x1025D0` | `.text` | HUD weapon-slot info: ctrl `+0x4C/+0x50/+0x54/+0x58`, `+0x5C/+0x60` | VERIFIED |

## Appendix B — confidence summary

| Claim | Marker |
| --- | --- |
| `WeaponModel` vftable RVA, one virtual (dtor), fixed size `0x48` | **VERIFIED** |
| payload at `+0x48`, size `0x48 + Σ`, allocated via pool `0x201460` | **VERIFIED** |
| 4 templates in the weapon manager at `+0x38..+0x158` | **VERIFIED** |
| `.wpb` values are **not** stored flat (96 B or 80 B contiguous) at runtime | **VERIFIED (negative)** |
| values are re-encoded as typed members `{hdr, f32@+0x08, meta@+0x0C}` in `0x18`+ slots | **INFERRED (strong)** |
| the specific offsets of `lead/scope/range/startAng/speed` | **UNKNOWN** |
| runtime units of `lead`/`startAng` (frames? radians?) | **UNKNOWN** |
| `WeaponEx*Ctrl` field map and 16×`0x68` slot array | **VERIFIED** |
| slot layout: pos `+0x00`, heading `+0x10/+0x14`, vec4 `+0x20`, state `+0x50` | **VERIFIED** |
| slot `+0x54..+0x67` contents | **UNKNOWN** |
