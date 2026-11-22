# ACZ / AC8PropNav — reverse-engineering report

Author: `bin-recon`. Everything below is either **VERIFIED** (I read it out of a
binary/log with the commands shown) or **INFERRED** (a conclusion that fits the
evidence but was not directly observed). Nothing here is a guess dressed up as a
fact — the marker is always there.

Scope of this round (per Lead's re-prioritisation):

1. The **ACZ addon ABI** (community `version.dll` modloader) — the integration
   surface our mod should target. §1
2. **How the reference addons find game code** — AOB patterns, patch mechanics,
   version guards. §2
3. **acz.exe static analysis** and what we can get without unpacking. §3
4. The **AC8PropNav reference mod** on the UE5 title (loading chain + exact
   property/offset parity table). §4
5. Runtime hints from acz.exe plaintext. §5
6. What this means for ACZPropNav. §6
7. Reproduction commands and artifacts. §8–§9

---

## 0. TL;DR

* The ACZ community loader is **structurally identical in spirit** to the AC8
  one: a proxy DLL dropped next to `acz.exe` (`version.dll`) loads addon DLLs
  from `ACZero\mods\`, each exporting exactly one symbol **`AczAddonInit`**,
  and hands them a small **context struct**: game directory at `+0x08`, the
  `acz.exe` base address at `+0x10`, and a **printf-style log function** at
  `+0x28`. That is the entire ABI we must implement. §1
* ADDONS DO THE HARD WORK THEMSELVES: they parse the PE to find `.text`, run
  **text AOB scans with `??` wildcards**, allocate trampolines near the game
  code and patch a 5-byte `E9 rel32` jump, then `VirtualProtect` +
  `FlushInstructionCache`. The loader gives no read/write/hook/scan services.
  §1.7, §2
* **Big static win:** `acz.exe` ships **full MSVC RTTI in plaintext `.data`**
  even though `.text` is encrypted. I recovered **387 class names → 404
  vftables with RVAs** without unpacking, including `ObjPlane` (the aircraft
  object), `WeaponExQaamCtrl`, `WeaponExSaamCtrl`, `WeaponModel`,
  `EffRingBuffer<UEFF_MIS_POSDIR>`, `CIndicatorLockonInfo`,
  `CIndicatorRadarMissile`. This gives us a *name-based object locator* that
  needs no byte patterns at all. §3.6
* `acz.exe` is **PE32+/GDK** built, `.text` entropy 8.000 (encrypted) by a
  **SteamStub Variant 3.1 x64** stub in `.bind`; `.fptable` is a
  **0x100-byte zero-filled section** (NOT a function-pointer table — that
  hypothesis is dead).
* **The exe is now statically unpacked**: Steamless 3.1.0.5 (prebuilt CLI)
  removes `.bind` and decrypts `.text` (entropy 8.000 → 6.641), producing
  `work/acz_copy.exe.unpacked.exe` with **identical section RVAs**, so every
  RVA in this report applies to it. From it I recovered the **weapon-controller
  vtable family** (40 `WeaponEx*Ctrl` classes, shared base slots) and the
  per-weapon **seeker** functions with their range/FOV constants — the entry
  points for the missile-guidance hunt. §3.5.1, §3.7
* The AC8 reference mod's parity table is recovered in full: five properties,
  two write targets (class-default objects for player weapons, a DataTable for
  NPC weapons), `LockonRange` only via a live weapon-instance field. §4.2

---

## 1. ACZ addon ABI (community modloader) — the thing we must implement

### 1.1 Evidence base

Reference addons on disk (downloaded by another teammate, treated as read-only):

* `work/dl/a6/ACZ Enhanced Gunplay Mod/mods/more_bullets.dll` — 41,984 B,
  exports `AczAddonInit` @ RVA `0x13A0`, imports only `msvcrt.dll` +
  `KERNEL32.dll`, Mingw-w64 built (pseudo-reloc + "Mingw-w64 runtime failure"
  strings).
* `work/dl/a6/ACZ Enhanced Gunplay Mod/mods/maneuvers.dll` — 217,088 B,
  exports `AczAddonInit` @ RVA `0x1000`, imports only `KERNEL32.dll`, MSVC
  built (static CRT: `RtlVirtualUnwind`, `HeapAlloc`, `FlsAlloc`…), has the
  same zero-filled `.fptable` section as `acz.exe`.
* `more_bullets.ini` / `maneuvers.ini` — config, and the readme states:
  *"place `version.dll` in the ACzero.exe folder location … this allows loading
  of custom mod ARC files that are stored in the `\ACZero\mods\` folder"* and
  *"modloader.log (next to acz.exe)"*.

The loader binary itself (`version.dll`) is **not** on this machine and was not
obtainable this round (see §3.5 / §9 – network to raw.githubusercontent and
github release assets is unreliable here; GitHub's **API** works, so a release
asset is worth one more try).

### 1.2 Entry point — VERIFIED

```c
/* x64 Windows calling convention: context pointer in RCX */
extern "C" __declspec(dllexport) int AczAddonInit(void* host_ctx);
```

* Export name is exactly `AczAddonInit`, ordinal 1, in both addons.
* Both addons save `RCX` to a callee-saved register on entry and never look at
  `RDX`/`R8`/`R9`. **One argument, pointer to the host context.**
* Return value: both return an `int` in `EAX` (0 on the "switched off" paths,
  non-zero once patched). Nothing in the two addons reads a return from the
  loader, so the loader almost certainly ignores it. Return `0` on failure,
  `1` on success to be safe. **INFERRED** that the loader ignores it.

Evidence (more_bullets, RVA 0x13A0):

```asm
1800013a0  push r15 / r14 / r13 / r12 / rsi / rdi / rbp / rbx
1800013b3  mov  rsi, rcx                 ; rsi = host ctx
1800013b6  mov  rax, [rcx+0x28]          ; ctx->log_fn
1800013c1  mov  r9,  [rcx+0x8]           ; ctx->game_dir
```

### 1.3 Host context layout — VERIFIED offsets, unknown padding

| Offset | Type | Meaning | Proof |
| --- | --- | --- | --- |
| `+0x00` | ? | not read by either addon | INFERRED: version/size field |
| `+0x08` | `const wchar_t*` | **game directory** (e.g. `…\ACZero`, no trailing `\`) | `_snwprintf(dst, 260, L"%ls\mods\more_bullets.ini", ctx->8)` at more_bullets `0x1800013db`; maneuvers passes it straight to its path builder at `0x180001041` |
| `+0x10` | `HMODULE` / `uint8_t*` | **base address of `acz.exe`** | both addons read `[base+0x3C]` (e_lfanew) and walk the section table looking for `.text` |
| `+0x18` | ? | not read | unknown |
| `+0x20` | ? | not read | unknown |
| `+0x28` | function ptr | **log function, printf varargs** | `mov rax,[rcx+0x28]` … `call rax` with `rcx=fmt, rdx=msg` |

The two addons only ever touch `+0x08`, `+0x10`, `+0x28`. Everything else is
either absent or unused by them; do not rely on it.

**Log callback ABI — VERIFIED.** `more_bullets` log helper at RVA `0x1BA0`:

```asm
180001ba0  sub rsp, 0x238
180001bd4  mov edx, 0x200              ; 512-byte stack buffer
180001bdc  call 0x180003550            ; _vsnprintf(buf, 512, fmt, va_list)
180001be1  mov rax, [0x18000b080]      ; host ctx->log_fn, cached at init
180001bed  lea rcx, [rip+…]            ; "[more_bullets] %s"
180001bf4  lea rdx, [rsp+0x30]         ; formatted message
180001bf9  call rax
```

So the host logger is **printf-compatible varargs**, and addons are expected to
prefix their own message (`"[more_bullets] %s"`). The host presumably adds the
timestamp and writes to `modloader.log` next to `acz.exe` and/or stdout; that
part is the loader's, not the addon's (**INFERRED**, matches readme wording).

### 1.4 INI convention — VERIFIED

Both addons build `<game_dir>\mods\<dllname>.ini`
(`"%ls\mods\more_bullets.ini"`, and maneuvers builds the same pattern with its
own name) and read it with `GetPrivateProfileIntW` / `GetPrivateProfileStringW`
/ `GetPrivateProfileSectionW` / `GetPrivateProfileSectionNamesW`.

* The ini name tracks the DLL name (`more_bullets.dll` ↔ `more_bullets.ini`).
* Format is plain Win32 INI: `[Section]`, `Key=Value`, `;` comments.
* Both read the ini **again later** (more_bullets at RVA 0x2310 when a mission
  loads; maneuvers per its docs "re-read while the game runs"). So an addon may
  re-read its ini whenever it wants — the loader only supplies the directory.
* Values are clamped **by the addon**, not the loader (more_bullets clamps
  `ExtraTables` to `0..15` at `0x180001436`–`0x18000143e`).

### 1.5 Lifecycle — VERIFIED behaviour, INFERRED loader mechanics

* Called **once per process**, synchronously, from a loader thread, **after the
  game's `.text` has been decrypted** — proven by the fact that both addons
  immediately AOB-scan `.text` and patch it at init and they work.
* **No teardown/unload export** exists in either addon. If we need per-frame
  work we create our own thread or hook and never unload.
* The loader must therefore defer past the Steam-DRM stub: `version.dll` is
  statically imported, so its `DllMain` runs *before* the wrapper in `.bind`
  decrypts `.text`. **INFERRED**: the loader spawns a thread in `DllMain` that
  waits for the game to be ready (window/OEP/known module state) and only then
  enumerates `mods\*.dll` → `LoadLibrary` → `GetProcAddress("AczAddonInit")` →
  call. We cannot see the wait condition without the loader binary.
* **Consequence for us:** never scan `.text` from `DllMain`. Scan from
  `AczAddonInit`, and be defensive — if the pattern is not there, log and
  switch off (that is exactly what the reference addons do: `"OFF: %s (this
  acz.exe is not the version the mod was made for)"`).

### 1.6 Minimal C++ skeleton that compiles against this ABI

```cpp
// ACZPropNav.cpp — builds to ACZero\mods\ACZPropNav.dll
#include <windows.h>
#include <cstdarg>
#include <cstdio>

using AczLogFn = void (*)(const char* fmt, ...);

struct AczHostCtx {              // only these three fields are proven
    void*           reserved0;   // +0x00  unknown (size/version?)
    const wchar_t*  game_dir;    // +0x08  "…\ACZero"
    HMODULE         exe_base;    // +0x10  acz.exe base
    void*           reserved18;  // +0x18  unknown
    void*           reserved20;  // +0x20  unknown
    AczLogFn        log;         // +0x28  printf-style logger
};

static AczHostCtx* g_host;
static void logf_(const char* fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[1024]; va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap); va_end(ap);
    g_host->log("[ACZPropNav] %s", buf);
}

extern "C" __declspec(dllexport) int AczAddonInit(AczHostCtx* host) {
    g_host = host;
    logf_("v0.1.0  exe=%p  dir=%ls", (void*)host->exe_base, host->game_dir);
    // 1. read <game_dir>\mods\ACZPropNav.ini   (GetPrivateProfile*)
    // 2. locate the game code we need (see §2/§3.6), all from host->exe_base
    // 3. either hook the guidance function, or run our own thread that writes
    //    verified missile parameters — then log what was found
    return 1;   // 0 = switched off
}
```

Nothing else is required: no import of an SDK, no callback registration call,
no handshake. The context pointer is the whole interface.

### 1.7 What the loader does **not** give us

Neither addon calls any helper other than the logger. Both import
`VirtualProtect`, `VirtualAlloc`, `VirtualQuery`, `GetSystemInfo`,
`FlushInstructionCache` themselves. So the host has **no** documented
read/write/hook/pattern-scan service (at least none used here). Our mod must
carry its own primitives — which is exactly the layering `docs/ARCHITECTURE.md`
already assumes.

---

## 2. How the addons find and patch game code (the technique we copy)

### 2.1 Step 1 — get `acz.exe` base and its `.text` (both addons)

`ctx->exe_base` (`+0x10`) → `[base+0x3C]` = `e_lfanew` → PE header → section
table → compare each 8-byte section name with `".text"`
(`movabs $0x747865742e`). If absent: `"the exe has no .text section"`.
Then scan `[VirtualAddress, VirtualAddress+VirtualSize)` of that section.
Because the PE header and section table are complete **before and after**
decryption, this works for an encrypted `.text` too — no packing check needed.

### 2.2 Step 2 — text AOB scan with `??` wildcards

`maneuvers.dll` contains its own pattern compiler at RVA `0x10E0`:

* input is an **ASCII string** like `"?? ?? ?? ?? ?? 48 89 54 24 10 …"`;
* `??` = wildcard byte (mask 0), hex pair = exact byte;
* max 96 (`0x60`) pattern bytes; bytes and mask stored in two parallel arrays;
* then a byte-wise scan of the target range, first match returned.

`more_bullets` keeps its four patterns as `.rdata` strings and compiles them the
same way.

### 2.3 Every concrete pattern found — VERIFIED, extracted verbatim

`more_bullets.dll` (scan order = spawn, hit, move, manager; see §2.5):

```text
?? ?? ?? ?? ?? 48 89 54 24 10 55 57 41 55 41 57 48 8D 6C 24 B8 48 81 EC 48 01 00 00 0F B6 05 ?? ?? ?? ?? 48 8B F9 8B 89 38 07 00 00
?? ?? ?? ?? ?? 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 B8 FE FF FF 48 81 EC 10 02 00 00 C5 F8 29 70 B8
?? ?? ?? ?? ?? 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 70 0F B6 05 ?? ?? ?? ?? 48 8D 59 50
?? ?? ?? ?? ?? 20 45 33 C0 C6 41 28 3C 48 8D 05 ?? ?? ?? ?? 4C 89 01 4C 89 41 08 48 8B D9 4C 89 41 10 4C 89 41 18 4C 89 41 20 48 89 41 38
85 B7 38 07 00 00 0F 84          ; fragment locating the bullet manager's inner loop
```

`maneuvers.dll`:

```text
?? ?? ?? ?? ?? 48 89 74 24 20 57 48 81 EC A0 00 00 00 0F B6 81 32 04 00 00 48 8B F9 0F B6 F2
?? ?? ?? ?? ?? 41 57 48 8D A8 F8 FE FF FF 48 81 EC F0 01 00 00 C5 F8 29 70 B8 C5 F8 29 78 A8
?? ?? ?? ?? ?? FA 10 84 24 90 00 00 00 C5 F8 2F D8 4C 8B 84 24 A0 00 00 00 4C 8B DA
?? ?? ?? ?? ?? 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 30 48 8B 35 ?? ?? ?? ?? 44 0F B6 F2 C5 F8 29 74 24 20 C5 C8 57 F6 41 80 BC 36 B6 02 00 00 FF
?? ?? ?? ?? ?? 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 B0 48 81 EC 50 01 00 00 C5 FA 10 0D
C5 FA 10 9E 10 02 00 00 C5 7A 10 2D ?? ?? ?? ?? 0F B6 8E 47 02 00 00
```

Two things worth copying from this style:

1. **The leading `?? ?? ?? ?? ??` is deliberate**: the patch overwrites exactly
   those 5 bytes with `E9 rel32`, so the signature still matches after the
   hook is installed (idempotent / re-entrant loading).
2. Patterns are long (25–60 bytes) and anchor on **prologue + a nearby `movzx`
   of a `this`-relative field** — that is what makes them survive minor
   rebuilds. Signs of AVX (`C5 F8`/`C5 FA`/`C5 C8`) mean the game is built with
   AVX float code — relevant for us when we write signatures.

### 2.4 Step 3 — patch mechanics (VERIFIED from imports + strings)

* 5-byte `E9 rel32` jump at the pattern site into a trampoline.
* Trampoline memory must be **within ±2 GB** of the game code — hence
  `VirtualAlloc` + `VirtualQuery` + `GetSystemInfo` scans and the failure
  message `"no memory within reach of the game code"`.
* `VirtualProtect` to `PAGE_EXECUTE_READWRITE` on the target pages,
  `FlushInstructionCache(GetCurrentProcess(), …)`, then restore protection;
  failures log `"could not patch the game code"`.
* The addon keeps the original bytes for the trampoline (`ready: spawn=+%llX
  hit=+%llX move=+%llX manager=+%llX` prints the resolved RVAs relative to the
  exe base — a nice debugging convention we should mirror).

### 2.5 Step 4 — version guard

Both addons treat "pattern not found" as "wrong game build" and switch
themselves off with a loud log line:

```text
OFF: %s (this acz.exe is not the version the mod was made for)
```

That is the whole version check: **no PE timestamp/hash compare** in either
reference addon (contrast with the AC8 mod in §4.5, which does fingerprint the
exe). Copy the graceful-off behaviour; consider adding the AC8-style
fingerprint for extra safety.

---

## 3. acz.exe static analysis

Target file (workspace copy, never modified):
`ACZero\acz.exe`, 4,046,008 bytes, SHA-256
`3802B0DC05C43DDA06BEA6D4124DCE611BD2956C7E9F27F9EB322615D42D5E5A`.

An older build at `…\ACE COMBAT 8\ACZero\acz.exe` is 4,037,304 bytes, SHA-256
`8AA8FFC9A185C58D45B34BC1116D8E1880EA785CC6FA3FA7D6A421FFFEB9C739`.
Note: the public ACZ RE write-up (`STanJK/AceCombatZeroReshade`) documents
SHA-256 `43814D4B…` — **a third build**. Its RVAs are therefore *not* safe to
copy for us.

### 3.1 Sections / entropy — VERIFIED (`tools/re/pe_report.py`)

| Section | RVA | vsize | raw | entropy | note |
| --- | --- | --- | --- | --- | --- |
| `.text` | `0x00001000` | `0x27B92F` | `0x27BA00` | **8.000** | encrypted |
| `.rdata` | `0x0027D000` | `0x08E4C0` | `0x08E600` | 5.376 | plaintext: command names, ini paths |
| `.data` | `0x0030C000` | `0x034D6D7C` | `0x01B600` | 2.147 | plaintext: **RTTI names + vftables** |
| `.pdata` | `0x037E3000` | `0x00018144` | `0x00018200` | 6.303 | exception unwind |
| `.fptable` | `0x037FC000` | `0x00000100` | `0x00000200` | 0.000 | **all zero** |
| `_RDATA` | `0x037FD000` | `0x00015910` | `0x00015A00` | 5.600 | |
| `.xbld` | `0x03813000` | `0x000000E1` | `0x00000200` | 2.984 | GDK build stamp |
| `.rsrc` | `0x03814000` | `0x00042660` | `0x00042800` | 3.985 | version info + manifest |
| `.reloc` | `0x03857000` | `0x00009F58` | `0x0000A000` | 5.444 | |
| `.bind` | `0x03861000` | `0x00039248` | `0x00039248` | **7.960** | Steam-DRM stub |

Entry point `0x3861310` is inside `.bind`; image base `0x140000000`;
`SizeOfImage` `0x389A000`; PE32+, linker 14.x.

### 3.2 Version info and build provenance — VERIFIED

`.rsrc` `VS_VERSION_INFO` (UTF-16):

```text
FileDescription  ACE COMBAT ZERO: THE BELKAN WAR
FileVersion      2.0.0.1
ProductName      ACE COMBAT ZERO: THE BELKAN WAR
ProductVersion   2.0.0.1
LegalCopyright   BANDAI NAMCO Entertainment Inc.
```

`.xbld` (a Microsoft **GDK** build stamp, plaintext):

```text
_xbld_edition_build=GRDK,0x65F41EA9
```

So the port is a **Microsoft GDK ("GRDK") x64 title** — which explains
`_RDATA`, `.xbld`, and the huge `.data` BSS (`vsize` 55 MB). On top of that
sits a Steam DRM wrapper (`.bind`).

### 3.3 `.fptable` is empty — the "function pointer table" hypothesis is dead

`acz.exe`, `maneuvers.dll`, `AVRT.dll` and `AC8PropNav.dll` **all** carry a
section named `.fptable`, always `vsize 0x100`, always **512 zero bytes**
(0 non-zero bytes, checked with `tools/re/pe_report.py` and a direct scan).
It carries no pointers and cannot be used as a function table. It is an
artifact of some common post-processing/build step (exact tool unidentified).

### 3.4 Old build vs new build — VERIFIED

Per-section raw diff (`tools/re/pe_report.py <new> <old>`), old = the
`ACE COMBAT 8\ACZero` copy:

| Section | raw size | RVA shift | changed bytes |
| --- | --- | --- | --- |
| `.text` | `0x27BA00`→`0x27A000` | same | 2,586,692 (different code build) |
| `.rdata` | `0x8E600`→`0x8E000` | `0x27D000`→`0x27B000` | 306,766 |
| `.data` | same | `0x30C000`→`0x309000` | 20,677 |
| `.pdata` | `0x18200`→`0x18000` | `0x37E3000`→`0x37E0000` | 44,074 |
| `.fptable` | same | shifted | **0** |
| `_RDATA` | same | shifted | **0** |
| `.xbld` | same | shifted | **0** |
| `.rsrc` | same | shifted | 12 |
| `.reloc` | same | shifted | 23,804 |
| `.bind` | same | `0x3861000`→`0x385D000` | 218,698 |

Conclusion: the two builds differ by **a real code delta of ~0x2000 bytes in
`.text` plus a different wrapper payload**, and every RVA above `.text` shifts
by `-0x2000`/`-0x3000`. So (a) RVAs are build-specific — always scan, never
hard-code, and (b) `.bind` differs too, i.e. the Steam wrapper is re-generated
per build.

### 3.5 Wrapper: SteamStub Variant 3.1 (x64), unpacked successfully

Evidence for SteamStub-family DRM: entry point in `.bind`, section name
`.bind`, encrypted `.text` (entropy 8.0), `.bind` itself high entropy (7.96),
wrapper codes the whole image (`.reloc`, `.pdata` differ), original imports
resolved before the stub runs. This is the classic Steam DRM wrapper
(SteamStub). Which *variant* (2.x/3.0/3.1) cannot be settled without running a
Steamless-style parser over the `.bind` payload — see status below.

**Static unpack: SUCCESS — SteamStub Variant 3.1 (x64) confirmed, `.text`
decrypted.** See §3.5.1. The `variant` question is answered: it is the
**3.1 x64** SteamStub, handled by `Steamless.Unpacker.Variant31.x64.dll`.

#### 3.5.1 Unpack procedure (reproducible) and verification

Building Steamless from source is impossible here (no .NET SDK: `dotnet --info`
→ "No SDKs were found"; no .NET Framework 4.5.2 targeting pack; no VS MSBuild;
`git clone` dies with `schannel: SEC_E_NO_CREDENTIALS`). The **prebuilt**
release works because the .NET Framework 4.x *runtime* is present:

```powershell
# download via the GitHub API (only api.github.com is reliable from this box)
#   … → work/tools/Steamless-release.zip   (v3.1.0.5, 610,646 B)
Expand-Archive work\tools\Steamless-release.zip work\tools\steamless-bin
Copy-Item ACZero\acz.exe work\acz_copy.exe                 # never touch the install
work\tools\steamless-bin\Steamless.CLI.exe work\acz_copy.exe
# → work\acz_copy.exe.unpacked.exe  (3,811,952 B)
```

Tool output (verbatim, key lines):

```text
[Steamless.Unpacker.Variant31.x64] File is packed with SteamStub Variant 3.1 (x64)!
 Step 1 - Read, decode and validate the SteamStub DRM header.
 Step 2 - Read, decode and process the payload data.  --> File has payload data!
 Step 3 - Read, decode and dump the SteamDRMP.dll file. --> File has SteamDRMP.dll file!
 Step 4 - Handle .bind section. Find code section. --> .bind section was removed from the file.
 Step 5 - Read, decrypt and process code section. --> .text section is encrypted.
 Step 6 - Rebuild and save the unpacked file. --> Unpacked file saved to disk!
[Steamless] Successfully unpacked file!
```

Verification of `work/acz_copy.exe.unpacked.exe`:

| Check | Result |
| --- | --- |
| `.text` entropy | **8.000 → 6.641** (plaintext x86-64) |
| entry point | `0x22F638` — inside `.text` (was `0x3861310` in `.bind`) |
| section RVAs | **identical to the packed file** (`.text` 0x1000, `.rdata` 0x27D000, `.data` 0x30C000, …) — so every RVA in this document stays valid |
| `.bind` | removed |
| imports | 20 DLLs intact |
| `.pdata` | 8,219 functions with unwind info |
| `.rdata/.data/.rsrc` | unchanged (plaintext already) |

This means **all the RTTI/vftable RVAs in §3.6 are directly usable against the
unpacked image**, and byte signatures generated from the unpacked image (§3.7)
are valid against the **live, decrypted process** — which is what our addon
sees at `AczAddonInit` time.

**Runtime alternative** (no unpacker needed, and still the better *product*
path) is the reliable one:

1. Our own addon DLL is already running inside the decrypted process at
   `AczAddonInit` time (§1.5). It can dump `acz.exe`'s **decrypted** image
   (read `.text` at `ctx->exe_base + rva`, `VirtualProtect` not even needed for
   reading) to `ACZero\mods\ACZPropNav.dump.bin` once, guarded by a config key.
   This gives a perfect on-disk image for Ghidra/IDA with the **same RVAs the
   live process uses** — better than Steamless output for our purpose, and it
   needs no separate dumper, no `CreateProcess` games, no import rebuilding.
2. Fallback for a pure external dump: create the process suspended, wait until
   `.text` entropy drops / a known decrypted prologue appears at the OEP,
   `VirtualProtect` + `ReadProcessMemory`, then run Scylla to rebuild imports.

### 3.6 Static RTTI map — VERIFIED (biggest static win)

`.data` is plaintext and contains **complete MSVC x64 RTTI**. Using
`tools/re/rtti_scan.py` (vftable[-8] → Complete Object Locator → type
descriptor → mangled name) I resolved **387 / 387 type descriptors and 404
vftables** with zero unpacking.

Machine-readable list: `work/acz_rtti.tsv` (`name<TAB>vftable_rva<TAB>col_rva`).

Selected entries relevant to a guidance mod (RVA = VA − `0x140000000`):

| Class (mangled) | vftable RVA | Why it matters |
| --- | --- | --- |
| `.?AVObjPlane@@` | `0x002AD4D0` | the aircraft object class — identify any live plane/missile owner by vptr |
| `.?AVWeaponModel@@` | `0x002AE790` | weapon model/visual object |
| `.?AVWeaponRef@@` | (in TSV) | weapon reference/handle |
| `.?AVWeaponExFireCtrl@@` | `0x002AF470` | generic weapon fire control |
| `.?AVWeaponExQaamCtrl@@` | `0x002AF740` | QAAM seeker behaviour |
| `.?AVWeaponExXlaaCtrl@@` | `0x002AF560` | XLAA |
| `.?AVWeaponExSaamCtrl@@` | `0x002AFAE8` | SAAM |
| `.?AVWeaponExSffsCtrl@@` | `0x002AF380` | SFFS |
| `.?AVWeaponExLagmCtrl@@` | `0x002AF5D8` | LAGM |
| `.?AVWeaponExXagmCtrl@@` | `0x002AF980` | XAGM |
| `.?AVWeaponExNpbCtrl@@` | `0x002AF9F8` | NPB |
| `.?AVWeaponExRclCtrl@@` | `0x002AFA70` | RCL |
| `.?AVWeaponExBombCtrl@@` | `0x002AFC50` | bomb |
| `.?AV?$EffRingBuffer@UEFF_MIS_POSDIR@@$0IAA@@@` | `0x002ACC68` | **missile position/direction ring buffer** (effect system) |
| `.?AVCIndicatorRadarMissile@…` | `0x002AE360` | HUD missile blips |
| `.?AVCIndicatorLockonInfo@…` | `0x002ADDD0` | HUD lock-on indicator |
| `.?AVCIndicatorHudWeaponRange@…` | `0x002ADD18` | HUD weapon range ring |

40 of the 387 classes are `*Ctrl` game-logic classes; the whole list also
contains `CPropertyAircraft`, `CPropertyMission`, `CMissionControl*`,
`ObjPlane`, `AcmModel`, … (see TSV).

**How to use this without any AOB scan (recommended primary locator):**

1. At init, take `ctx->exe_base`, read the PE, and read the plaintext `.data`
   section — the same RTTI chain as `rtti_scan.py`, but on the *live* image.
   Build `map<name, vftable_VA>` at runtime (no hard-coded RVAs, build-proof).
2. Walk the game's object manager / scan writable heap for objects whose first
   qword equals one of those vftable VAs → you have typed objects
   (`ObjPlane`, missile/weapon controllers, `WeaponModel`) with no pattern
   matching at all.
3. For the flight/guidance *code* we still need the decrypted `.text` (dumped
   once, §3.5) to map vtable slots → functions; after that, hooks can be
   installed by slot index at runtime, which is far more robust than a byte
   pattern.

### 3.7 Weapon-controller vtables and guidance candidates — VERIFIED structure, candidates marked

With `.text` decrypted, the RTTI vftables resolve to real functions
(`tools/re/dump_vtable.py <unpacked> <rtti.tsv> --grep <class>`).

**The 40 `*Ctrl` weapon classes share a base vtable prefix.** Slot numbers are
stable across the family (this is the base class's virtual layout):

| slot | shared implementation | meaning (inferred from call sites) |
| --- | --- | --- |
| 2 | `0x092EA0` (size 0x52C) | per-frame/base update |
| 3 | `0x0933D0` (0xE4) | auxiliary |
| 4 | `0x0A1AC0` (0xC8) | accessor |
| 6 | `0x093A40` (0x72) | small helper |
| 7 | per class | weapon-specific "arm/acquire" |
| **8** | **per class** | **seeker / target update (the interesting one)** |
| 9 | `0x09DBB0` for all homing missiles | **salvo update dispatcher** |
| 10 | `0x0943E0` for all homing missiles | shared post-update (effects/trail: uses 50/100/512) |

Homing-family slot 8 (all VERIFIED RVAs):

| class | slot 7 | **slot 8 (seeker)** | slot 9 | slot 10 |
| --- | --- | --- | --- | --- |
| `WeaponExQaamCtrl` | `0x09F040` | **`0x09F230`** (0x436) | `0x09DBB0` | `0x0943E0` |
| `WeaponExSaamCtrl` | `0x09E620` | **`0x09E990`** (0x545) | `0x09DBB0` | `0x0943E0` |
| `WeaponExXlaaCtrl` | `0x09E590` | `0x09E610` | `0x09DBB0` | `0x0943E0` |
| `WeaponExXmaaCtrl` | `0x09DFB0` | `0x09E580` | `0x09DBB0` | `0x0943E0` |
| `WeaponExXagmCtrl` | `0x09D800` | `0x09DA10` | `0x09DBB0` | `0x0943E0` |
| `WeaponExLagmCtrl` | `0x09BE80` | `0x09C490` | `0x09C020` | `0x0943E0` |

(`WeaponExFireCtrl` = the non-homing generic: slot 7/8/9 = `0x0A6CA0`/`0x0A7160`/`0x0A7440`.)

Evidence that slot 8 really is the seeker/target logic — the float constants
each function loads (extracted per function range):

* `WeaponExQaamCtrl` slot 8: `180` (deg↔rad), `2500` (range), `37` (cone°?),
  `20.48`, `0.2` (retarget interval), `0.5`, `10`
* `WeaponExSaamCtrl` slot 8: `180`, `90` (FOV°), `3000` (range), `15`, `1.5`,
  `0.9997` (≈cos 1.4°), `23.04`, `50.176`
* `0x9DBB0` (slot 9) is a 16-iteration loop over `this+0x80`, stride `0x68`,
  gated by a bitmask at `this+0x5c`, calling `0x091DC0` per live missile —
  i.e. "update every missile of this salvo". **This 16-slot/0x68-stride array
  at `this+0x80` is the per-weapon missile instance array** — a strong object
  locator for us.

**Byte signatures** (generated from the unpacked image with `??` for
RIP-relative displacements and rel32; valid against the live decrypted
process, and they do **not** start with the 5 wildcards the community addons
use, so add 5 `??` if you intend to patch at the signature start):

```text
WeaponExSaamCtrl slot8 seeker   rva=0x09E990
  48 8B C4 48 89 58 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 C8
  48 81 EC 10 01 00 00 C5 F8 29 78 B8 C5 FA 10 3D ?? ?? ?? ??

homing slot9 salvo update       rva=0x09DBB0
  48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 40 C5 F8 29 74
  24 30 C5 FA 10 35 ?? ?? ?? ?? 48 8B E9 48 8D B1 80 00 00 00

base slot2 update               rva=0x092EA0
  48 8B C4 4C 89 48 20 4C 89 40 18 48 89 48 08 55 53 56 57 41 54

slot10 shared (effects)         rva=0x0943E0
  48 8B C4 57 48 81 EC C0 00 00 00 48 89 58 08 48 8B FA 48 89 68 10 48 8D
  A9 DE 00 00 00 48 89 70 18
```

**Shared seeker helpers — VERIFIED call set.** Both `WeaponExQaamCtrl` slot 8
(`0x9F230`) and `WeaponExSaamCtrl` slot 8 (`0x9E990`) call the *same* helpers,
i.e. this is the generic homing machinery that every homing weapon reuses:

```text
0x090510   takes two floats in XMM2/XMM3 + this; heavy SSE/AVX body -> best "compute/steer" candidate
0x0907E0  \
0x090A60   |  seeker support (called by both, in the same order)
0x090D00  /
0x0934C0   FName/object-index resolve (0x3FF mask, reads [this->0x66EE0]) — not math
0x035B50   index/name helper (0x3FF mask on an argument) — not math
0x093AC0   SAAM-only extra step
```

Plain `WeaponExQaamCtrl` calls 7 of these, SAAM 9 (it adds `0x031FC0`,
`0x093AC0`, `0x09EEE0`). None of the six shared helpers has a `.pdata` entry,
so they are small/leaf functions — which fits "math primitive".


**Status: the per-missile *motion* function is found (C1 below); the exact
write of the velocity vector is narrowed to it but not yet watched live.**
What is proven: these are the virtual methods of the game's weapon classes, the slots
are consistent across the family, and slot 8 carries per-weapon range/FOV
constants. What is *not* yet proven by observation: which store inside C1 is the
final heading/velocity write (the code path is proven to be the per-missile
update — see §3.7.1). The plain **MSL** has no
`WeaponEx*Ctrl` class (it is not an "EX" special weapon), so its guidance is in
a base class of this hierarchy or in a separate non-RTTI class — §3.7.1 narrows
this to the per-missile update path that all homing weapons share.

#### 3.7.1 Resolved: the per-missile update path (candidates C1–C3)

The `WeaponEx*Ctrl` RTTI chain is now known:

```text
WeaponExQaamCtrl  derives from  WeaponExCtrlBase  (+ WeaponRef at object offset 8)
    base[1] .?AVWeaponExCtrlBase@@  numContained=1 mdisp=0  pdisp=-1 vdisp=0
    base[2] .?AVWeaponRef@@         numContained=0 mdisp=8  pdisp=-1 vdisp=0
```

`WeaponExCtrlBase` and `WeaponRef` have **no vftable of their own** (no COL is
emitted for them), so there is no "base class slot 8" to dump. Instead the
per-frame path is:

```text
weapon update
  └─ slot 9  (0x09DBB0, all homing weapons)
        for i in 0..15:                       ; 16 missile slots
            if (*(u32*)(this+0x5C) & (1<<i)) == 0: continue
            slot = this + 0x80 + i*0x68       ; <-- missile instance array
            C1(this, i, slot, 2.0f)           ; 0x091DC0
```

**C1 — `0x091DC0`, the per-missile motion/orientation update. STRONGEST
CANDIDATE.**

* Called **once per live missile** by slot 9 of `WeaponExLasmCtrl`,
  `WeaponExQaamCtrl`, `WeaponExSaamCtrl`, `WeaponExXagmCtrl`,
  `WeaponExXlaaCtrl` (VERIFIED: the only two `E8` call sites of `0x91DC0` are
  inside `0x09D550` and `0x09DBB0`).
* Body (`.pdata`-split into `0x91DC0`/0x45, `0x91E05`/0xDE, `0x91EE3`/0x655 —
  one logical routine) contains exactly the shape asked for:
  * reads the per-missile slot: `vmovups (%r8)`, `movzbl 0x50(%r8)`,
    `vmovss 0x20(%r8)`;
  * position deltas: `vsubss (%r15), %xmm0` / `vsubss 0x4(%r15), …` /
    `vsubss 0x8(%r15), …` (self minus target triples);
  * **angle wrap**: `vcomiss 3.1415927` (`0x2D4658`) then
    `vsubss 6.2831855` (`0x2D46D4`) — i.e. wrap into [-π, +π];
  * **absolute-value mask**: `vandps 0x2D5990` = `0x7FFFFFFF ×4`;
  * normalize: `vsqrtss` + `vdivss` on a 3-vector;
  * orientation matrix build: a 3×4 block written at `[rbp+0x04]`…`[rbp+0x48]`
    (12 scalar stores, mix of zeros and sin/cos results);
  * heading scalars written back at `0x10(%r15)` / `0x14(%r15)`;
  * one indirect call `callq *0xA0(%rax)` (scene object / transform accessor).
* Signature — **unique in `.text`** (verified: exactly 1 occurrence, at
  `0x91DC0`; 41 bytes; no RIP-relative displacement inside the window so no
  wildcards are needed):

```text
C1  0x091DC0   (48 8B C4 C5 FA 11 58 20 89 50 10 48 89 48 08 55 53 56 57
                41 54 41 55 41 56 41 57 48 8D A8 A8 FE)
  rva=0x091DC0  sig: 48 8B C4 C5 FA 11 58 20 89 50 10 48 89 48 08 55 53 56 57
                    41 54 41 55 41 56 41 57 48 8D A8 A8 FE FF FF 48 81 EC 18 02 00 00
```

* Argument ABI (from the call site and prologue spill pattern):
  `void C1(WeaponCtrl* this /*rcx*/, int slotIndex /*edx*/,
  MissileSlot* slot /*r8, = this+0x80+index*0x68*/, float mode /*xmm3, = 2.0f*/)`
  — the prologue spills exactly `rcx`→`[rax+8]`, `edx`→`[rax+0x10]`,
  `xmm3`→`[rax+0x20]`, and the body reads the slot from `r8` without spilling
  it. Return value unused.

**C2 — `0x09E030`, the shared XMAA/XLAA homing body.** Tail-called (`E9`, not
`E8`) from `WeaponExXmaaCtrl` slot 8 (`0x09E580`+8) and `WeaponExXlaaCtrl`
slot 8 (`0x09E610`+8); it calls all four shared helpers
(`0x090510/0x0907E0/0x090A60/0x090D00`) and uses 2500 / 37 / 90. Helper for the
family's *seeker* layer, not the flight layer. Signature (56 B, unique):
RIP-disp `AC 64 23 00` must be wildcarded:

```text
C2  0x09E030  sig: 48 8B C4 48 89 58 10 48 89 78 18 55 41 54 41 55 41 56 41 57
                   48 8D 68 C8 48 81 EC 10 01 00 00 C5 F8 29 78 B8 C5 FA 10 3D
                   ?? ?? ?? ?? C5 78 29 40 A8 C5 78 29 48 98 C5 7A
```

**C3 — `0x09F230` (QAAM) / `0x09E990` (SAAM) slot 8.** Per-weapon seeker with
range/FOV constants (2500 m / 37°, 3000 m / 90°); unique signature for QAAM:
`48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 81 EC 10 01 00 00 C5 F8 29 78`.
Note `0x9E030` and `0x9E990` share their first 0x28 bytes — always take the
56-byte form, not the prologue.

**Misleading lead, recorded so nobody chases it:** `.rdata` `0x2D4100–0x2D4D80`
is a *compiler float literal pool* (thousands of sorted constants, incl. 45/55/
75/90/150/180/360/2000/2500/3000, `0.0174533`, `57.2958`). Those are shared
literals, **not** a vanilla weapon-parameter table — the ACZ values are not
stored contiguously there.

#### 3.7.2 Exact next check (to upgrade C1 from "strong" to "proven")

Attach x64dbg to the live game, `bp 0x140091DC0` (plus the base if ASLR is on:
compute `acz.exe` base + `0x91DC0`), then:

1. on first hit, single-step to the `vmovss` stores at `0x9234D`–`0x923D1` and
   `0x9221B`/`0x92240` and note which register/DLL slot each writes;
2. confirm the slot passed in `R8` equals `this+0x80+i*0x68` and that `RDX`
   counts 0…15 — that proves the instance-array layout;
3. set a **hardware write breakpoint** (`ba w4` in x64dbg) on the 3-float vector
   at `slot+0x00` / `slot+0x10` and observe which instruction is the one writing
   the missile's heading each frame;
4. if that instruction is inside C1 → C1 is the flight function and the PN
   override can replace it; if it is inside a *different* function reached from
   C1, record that RVA and use it instead (the CV in §10 then applies unchanged,
   just with a different signature).


### 3.8 How to mine this further (next pass)

1. Resolve the **base class** of the `WeaponEx*Ctrl` family from the RTTI
   `ClassHierarchyDescriptor` (`COL+0x10` → base-class-array) and dump *its*
   vtable; slot 8 there is the generic (MSL) seeker.
2. For slot 8 of QAAM/SAAM, follow the called functions and look for the
   write into the missile's direction/velocity triple — that is the function to
   emulate/replace with PN.
3. Cross-check with the `ObjPlane` vtable (`0x2AD4D0`): its update method gives
   the aircraft's position/velocity/orientation, needed as PN inputs.
4. Everything above can be done statically now (unpacked image), then
   re-located at runtime by RTTI + a short signature, which is exactly the
   design in §6.

---

## 4. AC8PropNav reference mod (ACE COMBAT 8, UE5) — the spec we mirror


Read-only source: `…\ACE COMBAT 8\Game\Binaries\Win64\`
(`AC8PropNav.dll` 163,328 B, `AC8PropNav.ini` 7,360 B, `AC8PropNav.log`
365,631 B, `AC8PropNav.key.log`, `AVRT.dll`, `dwmapi.dll`, `ue4ss\`).

### 4.1 Loading chain — VERIFIED

```text
AceCombat8.exe
 ├─ static import: dwmapi.dll        → UE4SS proxy (PDB path …\RE-UE4SS\…\dwmapi.pdb)
 │                                    → LoadLibraryW("UE4SS.dll") → ue4ss\UE4SS.dll
 │                                    → UE4SS Lua mods listed in ue4ss\Mods\mods.txt/json
 │                                    (AC8PropNav is NOT in that list — checked)
 └─ static import: AVRT.dll          → custom proxy next to the exe (111,616 B)
                                      exports Av* (lazy-fowards to the real system avrt.dll)
                                      + watcher: "AVRT host ready - watching for
                                        AC8PropNav.dll / AC8Core_NNN.dll"
                                      → LoadLibraryW("AC8PropNav.dll")
                                      → GetProcAddress("Ac8CoreRun")
                                      → calls Ac8CoreRun()
                                      (probe variant: export Ac8ProbeRun → AC8Probe.log)
```

* `AC8PropNav.dll` exports exactly one symbol: **`Ac8CoreRun`** @ RVA `0x1000`.
* UE4SS is present but unrelated to this mod; the mod's own `AVRT.dll` proxy is
  the loader. Its host messages: `[HOST] loading core #N`,
  `[HOST] calling Ac8CoreRun()`, `[HOST] done - drop a higher-numbered
  AC8Core_NNN.dll to run again` — i.e. **numbered reload without restarting**,
  a UX worth copying.
* AC8PropNav itself logs `[CORE] Ac8CoreRun() entered  文件 AC8PropNav.dll
  代次 #0` — it is told which file/generation it is.

### 4.2 Functional parity table — VERIFIED (mod's own log + descriptor table)

The DLL carries a 5-entry descriptor table in `.data` at RVA `0x27010`, stride
`0x28`: `{ char* name; /*+0x08 pad*/ u32 cdo_off; u32 row_off; u32 ?; u32 ?; u32 ?; }`
with the count at `0x27008`. The values below were confirmed twice: from the
table bytes and from the mod's own per-field log
(`[CORE] 字段 X  CDO偏移 0x…  行偏移 0x…  按名解析`).

| Property | AC8 **CDO** offset | AC8 **DataTable row** offset | Unit | Range / clamp | Physical effect |
| --- | --- | --- | --- | --- | --- |
| `HomingForesightAmount` | `+0x454` | `+0x24` | ratio 0…1 | none applied (INI value used as-is; 0 = pure pursuit, 1 = full lead) | lead/PN blend: aim point = target + targetVel·tGo·amount |
| `MaxHomingAngle` | `+0xA18` | `+0x2C` | degrees | none applied | seeker FOV cone; target outside cone loses guidance even with lock. Vanilla MSL 75 / QAAM 120 / LAAM 90 / HVAA 65 / 4AAM 65 |
| `MaxRotationAngle` | `+0xA1C` | `+0x30` | deg/s | none applied | max turn rate of the missile. Vanilla MSL 40 / QAAM 75 / LAAM 150 / HVAA 55 / 4AAM 45 |
| `LifeTime` | `+0x420` | `+0x10` | seconds | none applied | life = range: range ≈ lifetime × speed. Vanilla MSL 9 s ≈ 9.7 km |
| `LockonRange` | `+0x46C` | **none (`0x0`)** | metres | sanity check only: refuses to write if the *current* value looks implausible | HUD lock ring distance; **player only**; written to the live **weapon instance**, `param+0x88`; takes effect after one weapon switch |

Notes, all VERIFIED from the mod's log/disassembly:

* Units/semantics are corroborated by the shipped `AC8PropNav.ini` comments
  (bilingual, same numbers).
* **No clamping** of the five values: I found no `minss/maxss/comiss` guard
  around the writes (the only float compares in the DLL are in the
  float→string formatter and in the LockonRange plausibility test). Unlike the
  ACZ addons, AC8PropNav trusts the INI.
* **Two write targets, deliberately:**
  * **CDO** = class default object of a *player* weapon blueprint class
    (`BP_plwp_msl_a0_C`, `BP_plwp_4aam_a0_C`, … `plwp` = player weapon).
  * **DataTable rows** of `DT_NPC_Weapon` — the row struct must be
    `LiveNpcWeaponDataTable`, otherwise the mod **refuses to write**:
    `★★ DT_NPC_Weapon 的行结构体不是 LiveNpcWeaponDataTable ⇒ 拒绝写数据行
    （+0x24 的偏移假设不成立）`. This is a *type guard on a data layout* — copy
    that idea.
* Every write is logged per target:
  `[CORE]   [行 ] Missile_000  +0x24 = 0.300` and `[CDO] …  +0x454 = …`.
* Values are *idempotent*: the log distinguishes 实写 (written) from
  已是目标值 (already at target) — it re-bootstraps when the count of targets
  or the world instance changes.

### 4.3 Model matching, and player-vs-enemy separation — VERIFIED (mechanism), partially inferred (labels)

* A token table lives in `.rdata` at RVA `0x1A49C…0x1A5B0`:
  `qaam hcaa laam hvaa hpaa 4aam 6aam 8aam 2aam saam sasm exsam stgr ciws
  aagun flakgun lasm howitzer artillery tankgun bomb rocket missile flare
  ground vessel norn heli wingman plane` (`plwp` separately at `0x1A840`).
* Matching is a **case-insensitive substring test of the INI key against the
  object's class name**, longest key first (the INI documents this:
  `4aam` must not be stolen by `aam`). The INI also has a fallback legacy form
  `msl = 0.8`.
* Player/enemy split:
  * `[player]` → matched against **blueprint CDOs** whose class names contain
    the key plus `plwp`/`plane`; written at the CDO offset.
  * `[enemy]` → matched against **`DT_NPC_Weapon` rows**; written at the row
    offset. `LockonRange` simply has no row offset (see table) because "enemies
    do not lock on" (INI comment), and the mod's CT diagnostics state
    `ini 里没有任何型号设了 LockonRange ⇒ 这一项不会有任何作用` when unused.
* Diagnostics prove the matching is name-based, not table-based:
  `★见到武器 CDO 却没匹配上，前几个类名：LiveWeaponBase LiveExplosiveWeapon
  LiveArtillery LiveAttachedWeapon （ini 里要能对上型号关键词才会被收）`.

### 4.4 `LockonRange` mechanism in detail — VERIFIED from log + code

It is **not** a data-table write. The mod:

1. Anchors a global with a **hard-coded code signature**: logs
   `CT 指令 exe+0x4958E67  字节 48 8B 05 7A 64 9B 09` → checks the opcode is
   `mov rax,[rip+d]`, computes the global address `exe+0xE30F2E8`
   (`0x180000000`-relative for that build), and calls it the
   `AC8C_WorldLoad` anchor.
2. Walks a 5-step pointer chain, logging every step:
   `global → +0x1D8 → +0x38 → +0x0 → +0x30 → +0x338`.
3. Has a **fallback static pointer** `exe+0xE371938` for when the main anchor
   breaks (and says so: `主入口（AC8C_WorldLoad）不通，正在用备用静态指针入口`
   + `若之后 LockonRange 不生效，请优先怀疑 exe 构建已变`).
4. Then enumerates "player weapon instances" (records of `0xA0` bytes) and
   writes `param+0x88`, with:
   * a plausibility gate (`不像锁定距离 ⇒ 不写（布局可能变了）`),
   * write-then-read-back verification (`✓` / `★写后读回不一致`),
   * and the user-facing note that a **weapon switch** latches the value.

Lesson for us: a *live-instance* property needs an anchor + fallback + readback
verification; a *static parameter* does not (§4.2).

### 4.5 Logging, config reload, build fingerprint — VERIFIED

* Files: `AC8PropNav.log` (cap 500 KB, truncated-and-rewritten with a banner
  `---- 日志已超过上限，清空重写 ----`), `AC8PropNav.key.log` (cap 250 KB,
  keeps the most recent half, banner `---- key.log 超过 250KB：已丢弃较早的一半
  ----`). Both next to the exe, timestamps `[HH:MM:SS]`.
* Tags: `[CORE]` for everything, `[FAULT] 异常码 0x… 出错位置 … 执行 …` for
  SEH-caught exceptions (it even logs third-party module faults).
* Startup banner: `==== AC8PropNav v2.3.7 ====`, exe base, image size,
  `GObjects ok：对象槽位 N`, `INI: [player] 11 条 [enemy] 1 条`, then
  bootstrap passes with a numbered `引导 #N` and a diagnosis block.
* **INI hot-reload**: `---- 检测到 INI 有改动，重新载入 ----`.
* **Build fingerprint guard**: `** exe 构建指纹不符（预期 ts=… si=…，实际
  ts=…）⇒ 拒绝对内存动手` — compares `TimeDateStamp` + `SizeOfImage` of
  `AceCombat8.exe` before touching memory. Copy this for ACZ.
* Rate-limit self-protection: `** 引导过于频繁（60 秒内 >25 次）⇒ 闸门放宽到
  5 秒` — an anti-thrash gate we should also have.

---

## 5. Runtime hints from `acz.exe` plaintext

From `work/acz_strings.txt` (73,079 strings; `.rdata`/`.data`/`.rsrc` are
plaintext). The most useful for us:

* **Config/ini files the game itself reads:**
  `settings/setting.ini`, `settings/dinput.ini`, `settings/dinputSample.ini`,
  `aczinit.txt` (next to the exe).
* **Engine command/console surface:** a long list of `verb_object` names, e.g.
  `select_isQuickMode`, `select_getSelectedWeapon`, `select_isWeapon`,
  `unlock_getAircraft`, `shop_buyExWeapon`, `shop_sellExWeapon`,
  `shop_initBuyExWeapon`, `EffDrawMissileSmoke`, `debugPrint`. This naming
  (`module_action`) is the engine's internal scripting API — a possible
  non-hook integration path later (call these through the engine instead of
  patching floats).
* **RTTI class names** — see §3.6 (this is the jackpot; 387 classes).
* Debug UI classes survive in the shipping build: `CDebugCtrl`,
  `CDebugModeAircraftSelect`, `CMainSelectDebug`,
  `CSelectMemorycardDebug` — suggests a debug menu may still be reachable.
* Weapon/guidance-relevant classes: `WeaponRef`, `WeaponModel`,
  `WeaponExFireCtrl`, `WeaponExQaamCtrl`, `WeaponExSaamCtrl`, `WeaponExXlaaCtrl`,
  `WeaponExLassm…`, `WeaponExBombCtrl`, `CIndicatorLockonInfo`,
  `CIndicatorRadarMissile`, `CIndicatorHudWeaponRange`,
  `EffRingBuffer<UEFF_MIS_POSDIR>`.
* Wrapper/DRM: `.bind` has no readable Steam strings; imports are the normal
  game imports. Version blob as in §3.2.

---

## 6. What this means for ACZPropNav

1. **Deliverable shape: an addon DLL + ini, with an optional own proxy.**
   `ACZero\mods\ACZPropNav.dll` exporting `AczAddonInit` is the community-
   compatible path; keep our own `version.dll`/`dinput8.dll` proxy as a fallback
   that constructs the same context struct and calls the same export. The
   context is small enough (§1.3) that we can synthesise it ourselves.
2. **Do not expect loader services.** Read/write/hook/scan are ours (§1.7).
3. **Locate objects by RTTI, not by hope.** Build a name→vftable map at
   runtime from the live `.data` (§3.6). That gives us `ObjPlane` (aircraft),
   `WeaponModel`, the `WeaponEx*Ctrl` classes and `CIndicatorLockonInfo`
   without a single byte pattern, and it is immune to wrapper/rebuild churn
   because nothing is hard-coded.
4. **Locate code by AOB + `??` wildcards, ≥25 bytes, anchored on a prologue
   plus a nearby `this`-relative `movzx`, with 5 leading wildcards** so the
   signature survives our own patch (§2.3). The game uses AVX, so include
   `C5 F8/F9/FA` prologue bytes when it helps disambiguate.
5. **Patch style:** 5-byte `E9 rel32` → trampoline allocated within ±2 GB,
   `VirtualProtect` → write → `FlushInstructionCache` → restore, keep original
   bytes, log resolved `exe+0x…` RVAs (§2.4).
6. **Parameters over hooks when possible.** AC8PropNav's *always-on* path is
   just writing verified floats into the parameter storage (CDO/table rows).
   For ACZ we should first find the missile parameter block(s) and try the same
   thing — it is crash-free, reversible, and needs no trampolines. The
   guidance *loop* replacement (real PN) comes second, once the dump exists.
7. **Mirror the AC8 UX because users already know it:** same five property
   names, same units and semantics, `[player]`/`[enemy]` sections, longest-key-
   first matching, `msl = 0.8` legacy form, blank = vanilla, bilingual
   comments, 500 KB/250 KB log pair, hot-reload, fingerprint guard, graceful
   "OFF" when the build does not match, and a numbered dev generation like
   `ACZCore_NNN.dll`.
8. **We must implement a type/layout guard like AC8PropNav's**
   `LiveNpcWeaponDataTable` check: refuse to write when the discovered struct
   is not what we expect, and say why in the log.
9. **Get a decrypted `.text` dump — from our own DLL, at init.** It is the
   cheapest, most reliable route to the missile-guidance function and it is
   already 90% written for us by working inside a decrypted process (§3.5).

---

## 7. Verified vs inferred — quick index

| Claim | Status |
| --- | --- |
| `AczAddonInit(void* ctx)`, one export, x64 cdecl/fastcall, called once after `.text` decryption | VERIFIED (exports, code), timing INFERRED from addon behaviour |
| ctx `+0x08` = wide game dir, `+0x10` = exe base, `+0x28` = printf logger | VERIFIED |
| ctx `+0x00/+0x18/+0x20` meaning | UNKNOWN |
| Loader is `version.dll` in `ACZero\`, addons from `ACZero\mods\`, log `modloader.log` | VERIFIED via readme/ini text; the binary itself not obtained |
| Addons AOB-scan `.text` with `??` patterns and patch with `E9 rel32` + trampolines in ±2 GB | VERIFIED (code, strings, imports) |
| Pattern strings in §2.3 are the real ones used | VERIFIED (they are the strings the scan call sites reference) |
| `acz.exe`: PE32+/GDK, `.text` encrypted, `.bind` wrapper, version 2.0.0.1 | VERIFIED |
| `.fptable` is a zero-filled 0x100 section, not a pointer table | VERIFIED |
| Full RTTI map 387 names / 404 vftables with RVAs | VERIFIED by tool; names/RVAs reproducible |
| AC8 loading chain via custom `AVRT.dll` proxy → `Ac8CoreRun` | VERIFIED (exports, imports, strings, PDB path of dwmapi) |
| AC8 parity table (5 properties × CDO/row offsets, units) | VERIFIED (descriptor table bytes + mod's own log + ini docs) |
| No clamping of AC8 values; `LockonRange` = live instance `param+0x88` | VERIFIED (code/log) |
| SteamStub variant number for acz.exe | **VERIFIED: Variant 3.1 (x64)** via Steamless |
| Static unpack of acz.exe possible here | **YES** — prebuilt Steamless 3.1.0.5 CLI; `.text` 8.000 → 6.641 entropy, `.bind` removed, RVAs preserved |
| Weapon `*Ctrl` vtable slots 2/3/4/6 shared; slot 8 per-weapon seeker | VERIFIED (vtable dumps + constants) |
| Which function writes the missile's flight/turn rate | **NOT YET IDENTIFIED** (slot-8 candidates listed) |
| Steamless buildable from source here | NO (no .NET SDK / targeting pack / VS MSBuild) — used prebuilt release instead |

---

## 8. Reproduction commands

All tools live in `ACZPropNav/tools/re/`; all outputs in `ACZPropNav/work/`.
`BASH` is unavailable in this environment (sandbox runtime error) — use `pwsh`.
Python: `C:\Users\silic\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe`.

```powershell
$py = "C:\Users\silic\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe"
$re = "…\ACZPropNav\tools\re";  $wk = "…\ACZPropNav\work"

# strings (UTF-8 + UTF-16LE, with file offsets) — plain `strings` mangles CJK
& $py "$re\extract_strings.py"  $exe 4 "$wk\acz_strings.txt"

# PE section entropy / zero-fill / old-vs-new diff
& $py "$re\pe_report.py"        $exe "$old"

# RTTI: names -> COL -> vftable RVAs (works on the encrypted .text)
& $py "$re\rtti_scan.py"        $exe --out "$wk\acz_rtti.tsv" --grep Missile

# IAT slot -> symbol (to annotate `callq *0x…` in objdump)
& $py "$re\iat.py"              $dll --map "$wk\iat.txt"

# find 8-byte pointers to a VA (descriptor tables, vftable->COL links)
& $py "$re\find_ptrs.py"        $dll 0x18001A440 …

# function index: .pdata boundaries + per-function string xrefs
& llvm-objdump -d --print-imm-hex $dll > "$wk\disasm.txt"
& $py "$re\build_func_index.py" $dll "$wk\disasm.txt" "$wk\strings.txt" "$wk\funcs.txt"

& llvm-readobj --coff-exports --coff-imports --sections $dll
```

Key facts used above and where they came from:

* AC8 descriptor table: `.data` RVA `0x27010`, stride `0x28`, count at
  `0x27008`; name lookup function at `0x180007700` (lower-cases and truncates
  to 39 chars); field printer at `0x1800086D5`.
* AC8 row-struct guard function at `0x180004624`;
  `LockonRange` instance writer around `0x180004850–0x180004B00`.
* more_bullets: init `0x1800013A0`, log `0x180001BA0`, pattern compiler/scan
  `0x1800010E0`-equivalent in maneuvers, patch `0x180002B10`, hook bodies
  `0x180001FC0/0x180002110/0x1800021C0/0x180002310`, ini reread `0x180002310`.
* maneuvers: init `0x180001000`, path builder `0x1800012B0`, pattern compiler
  `0x1800010E0`.
* AC8 `AVRT.dll` exports at RVA `0x1000…` each lazily resolve then tail-call
  the genuine system function.

---

## 9. Artifacts, and what I would do next

Produced in `ACZPropNav/work/` (all disposable):

| File | Contents |
| --- | --- |
| `ac8pn_disasm.txt`, `ac8pn_funcs.txt`, `ac8pn_strings.txt`, `ac8pn_pe.txt` | AC8PropNav.dll analysis |
| `mb_disasm.txt`, `mb_funcs.txt`, `mb_strings.txt`, `mb_iat.txt` | more_bullets.dll analysis |
| `mv_disasm.txt`, `mv_funcs.txt`, `mv_strings.txt`, `mv_iat.txt` | maneuvers.dll analysis |
| `acz_strings.txt`, `acz_rtti.tsv` | acz.exe plaintext strings + RTTI/vftable map |
| `acz_copy.exe`, `acz_copy.exe.unpacked.exe` | the unpacked image (Steamless output) + its input copy |
| `weapon_const_xrefs.txt`, `const_xrefs.txt`, `weapon_funcs.txt` | float-constant xref maps and the disassembly of the weapon functions |
| `dl/a6/…` | the A6 reference mod (untouched) |
| `dl/aczr/*` | the public ACZ RE docs + ReShade addon sources (fetched via GitHub API) |
| `tools/steamless*`, `Steamless-release.zip` | Steamless source/release used for the unpack |

Next steps I would take, in order:

1. **Find the plain-MSIL flight/guidance function** from the unpacked image
   (DONE: unpack), starting at the `WeaponEx*Ctrl` slot-8 seeker functions
   (§3.7) and their base class — resolve the base class vtable from the RTTI
   class-hierarchy descriptor, then follow the direction/velocity writes.
   Deliverable: one function + a short signature, plus the missile instance
   object layout (`this+0x80`, stride `0x68`, bitmask `this+0x5c`).
2. **Hook feasibility check**: with the missile instance array known, decide
   between (a) parameter patching, (b) replacing the seeker result, or
   (c) full PN override in a trampoline; measure which fields are read per
   frame. This is the actual PN integration design.
3. **Ask the community loader's author for the header** (or for `version.dll`)
   once the network allows; the ABI in §1.2–§1.4 is enough to *work*, but a
   header would settle `+0x00/+0x18/+0x20` and the discovery/teardown rules.
4. Confirm the "called after decryption" timing by instrumenting our own addon
   (log the first bytes of `.text` at `+0x1000` and check for a real prologue
   across several launches).
5. Only then: Ghidra headless on the unpacked image if deeper decompilation is
   wanted (it is not required for the steps above — `llvm-objdump` + the
   `.pdata`/RTTI tooling in `tools/re/` is enough and much faster).


---

## 10. How to finish the true-PN override in one more session

Everything below is concrete and ordered. Nothing here needs a new RE pass —
only live confirmation and the hook wiring.

### 10.0 Validity statement (read this before trusting any RVA)

All RVAs in this document are for **`ACZero\acz.exe`, 4,046,008 bytes**,
SHA-256:

```text
3802B0DC05C43DDA06BEA6D4124DCE611BD2956C7E9F27F9EB322615D42D5E5A
```

They are offsets into the **decrypted, live image** as well (the unpacked file
preserves section RVAs — §3.5.1), so `exe_base + RVA` is directly usable at
runtime.

If the game updates, **re-run the tooling instead of re-using constants**:

```powershell
& $py tools\re\pe_report.py ACZero\acz.exe                       # .text entropy still 8.0?
work\tools\steamless-bin\Steamless.CLI.exe work\acz_copy.exe     # new unpacked image
& $py tools\re\rtti_scan.py work\acz_copy.exe.unpacked.exe --out work\acz_rtti.tsv
& $py tools\re\dump_vtable.py work\acz_copy.exe.unpacked.exe work\acz_rtti.tsv --grep WeaponEx
& $py tools\re\owners.py work\acz_copy.exe.unpacked.exe work\acz_rtti.tsv --anchors 0x91DC0,0x9E030
& $py tools\re\xref_consts.py work\acz_copy.exe.unpacked.exe --consts 3.14159265,6.2831855
```

then re-derive the signature (§10.1) from the new image and re-check it is
unique. The `AczAddonInit` ABI (§1) does **not** change with game builds; only
these RVAs/signatures do.

### 10.1 Checklist

1. **Ship the signature, not the RVA.** Primary hook target = C1:

   ```text
   ; 41 bytes, unique in .text of 3802B0DC… (verified 1 hit), no wildcards needed
   48 8B C4 C5 FA 11 58 20 89 50 10 48 89 48 08 55 53 56 57
   41 54 41 55 41 56 41 57 48 8D A8 A8 FE FF FF 48 81 EC 18 02 00 00
   ```

   Because our detour overwrites the first 5 bytes, store the pattern with the
   **first 5 bytes wildcarded** (`?? ?? ?? ?? ?? C5 FA 11 58 20 …`) exactly
   like the community addons do (§2.3), so re-scanning after the hook is
   idempotent. Verify `hit_count == 1` and log the resolved `exe+0x…`.

2. **Hook with `HookArgIndex = 0`** (RCX = `this`, the `WeaponEx*Ctrl`). That is
   the only argument our `this`-based detour needs, and it is the argument both
   reference addons' style assumes. The other registers are **not** optional for
   C1 though: `RDX = slotIndex` and `R8 = missile slot pointer` are consumed by
   the body, so the trampoline must forward them untouched.
   * If our `FloatArg` mechanism is used, the 4th argument is `XMM3 = 2.0f`
     (a constant mode/scale, **not** `dt`) — so do **not** rely on it for a
     timestep; treat `HookArgIndex=0` + `FloatArg` as "log only".
   * If the detour ABI can only carry `this` (+float) and would clobber
     RDX/R8, either (a) write the detour as an **asm thunk** that spills
     `rcx/rdx/r8/xmm3` to a per-thread record and calls the C++ handler, or
     (b) hook the **caller** instead — see step 3.

3. **Caller-side fallback that needs only `this`:** hook `0x09DBB0`
   (`WeaponEx*Ctrl` slot 9, the salvo dispatcher, signature in §3.7). It is
   entered once per frame per weapon with `RCX = this`, and from `this` alone we
   can reproduce everything the callee gets:
   * live missile indices: `mask = *(u32*)(this + 0x5C)`, bits 0..15;
   * missile slots: `slot[i] = this + 0x80 + i*0x68`;
   * call C1 ourselves or patch the slot fields directly.
   This gives us per-frame enumeration of every missile of every weapon with a
   `this`-only detour — enough for the **parameter-patch** mode of the mod, and
   for PN if we also read the slot's pos/vel fields.

4. **`Probe = 1`** (no writes, log only):
   * confirm `signature found: 1 hit @ exe+0x91DC0`;
   * log, once per second: `this`, `mask`, live missile count, and for each live
     slot the three floats at `slot+0x00` (position, expected), `slot+0x10`,
     `slot+0x20` plus the bytes at `slot+0x50` (the per-slot flag byte C1 reads
     with `movzbl 0x50(%r8)`);
   * sanity: positions inside the map, missile count 0…16, slot pointers
     ascending by exactly `0x68`.
   * If this looks right, the instance layout is proven and no further RE is
     needed.

5. **`DryRun = 1`** (compute PN, log the command, write nothing):
   * for each live missile, find the target (may need the seeker path: slot 8 /
     `0x9E030` has the target index; until then, use the player's locked target
     from our own config fallback);
   * compute the PN command, log `LOS rate`, `closing speed`, `N`, the clamped
     turn per frame, and the *delta* to the slot's current heading;
   * compare the sign/scale of the delta against the in-game smoke trail
     bending. A sign flip or a 57.3× factor is the classic failure — the game
     works in radians (π/2π wrap constants at `0x2D4658`/`0x2D46D4` prove the
     heading is stored in radians).

6. **Then enable writes behind an explicit unlock** (`EnableWrites=1`), starting
   with `MaxRotationAngle`-style *parameter* patching, and only then the C1
   heading override. Always keep the read-back verification and refuse to write
   when the slot's `+0x50` flag byte or the `mask` says the slot is dead.

### 10.2 What to check in `ACZPropNav.key.log`

The key log must answer, without the main log:

```text
[PROBE] sig C1 hit=1 rva=exe+0x91DC0                      <- exactly one hit
[PROBE] slot9 hit=1 rva=exe+0x9DBB0                       <- caller fallback
[PROBE] this=0x…  mask=0x00FF  live=8                     <- 0..16, plausible
[PROBE] mis[0] pos=(…)  v=(…)  f50=0x01                  <- positions sane
[PN]    mis[0] losRate=…  vc=…  N=3.0  cmd=…deg          <- no NaN/inf
[PN]    mis[0] applied=…deg  readback=OK
[DRYRUN] would-write=0  frames=…                          <- dry run asserted
```

Fail conditions to look for: `sig hit=0` (build changed → §10.0), more than one
hit (signature too short), `live > 16` (mask/layout changed), `NaN` anywhere,
`busy`/`SEH` lines on level change, and any `readback mismatch`.

### 10.3 Fallback if the ABI guess is wrong

C1's argument use is proven from its **prologue spill pattern**
(`rcx`→`[rax+8]`, `edx`→`[rax+0x10]`, `xmm3`→`[rax+0x20]`, slot in `r8`), so a
wrong guess is unlikely — but if the live run shows garbage:

1. switch to the **asm-thunk** variant (spill `rcx/rdx/r8/xmm3`, no assumptions,
   log raw registers for one frame, then model them);
2. hook **`0x09DBB0`** instead (`this`-only, §10.1.3) and enumerate slots
   yourself — this bypasses the ABI question entirely;
3. if the slot layout differs (stride ≠ `0x68` or base ≠ `this+0x80`), re-derive
   it from `0x9DBB0`'s loop (`leaq 0x80(%rcx)` + `addq $0x68, %rsi` + `cmpl
   $0x10`) — those three constants are visible in its 40-byte body and are the
   ground truth;
4. if the missile heading is written somewhere other than C1 (see §3.7.2 step 3),
   use that RVA with the same checklist; only the signature in step 1 changes.

### 10.4 What NOT to do

* Do not hook `.text` during `DllMain` — only from `AczAddonInit` (§1.5).
* Do not hard-code RVAs from the *packed* file or from the older build
  (`…\ACE COMBAT 8\ACZero\acz.exe`) or from the STanJK repo's third build
  (`43814D4B…`) — all three differ (§3.4).
* Do not treat `.rdata 0x2D4100+` as a weapon table (§3.7.1) or `.fptable` as a
  function table (§3.3).
* Do not write to a slot before checking `mask bit` + `slot+0x50`; dead slots
  are the classic crash source.
