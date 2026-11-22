# PROBE_RUN_A — one read-only in-game run of the plain-MSL survey

Status: **implemented** (`src/core/probe.h`, `src/core/probe_run.cpp`, wired into
`Patcher::Start/Tick/Shutdown`). It is
enabled by `[runtime] Probe = 1` and never runs in a normal PN session.
*(The `probe_model.cmd` / `probe_model.ps1` toggle scripts that used to switch this on
have since been removed; the INI key alone is the switch.)*

Why it exists: the plain MSL is not a `WeaponEx*Ctrl`, never reaches the `0x90510`
integrator, and its pool node has **no per-frame transform writer**
(`docs/MSL_ADVANCE.md` §5). The remaining questions — where the runtime `.wpb` values
live and which world object/part is the flying missile — are answered by *observing*
three places at once for one sortie:

1. **the pool allocator** (which weapon records get allocated, with their sizes),
2. **the pool node pop** (which spawn site produced a node, and the node itself),
3. **the per-frame world tick** (the object/part transforms that actually move).

No game memory is written. The only mutations are the three code jumps below, which
`hook::Install`/`hook::Remove` create and restore exactly; there is no PAGE_GUARD, no
pointer chasing and no new thread (`docs/HANDOFF.md` §6.3).

---

## 1. What actually gets installed

| target | ABI at the hook | first bytes (from `work/acz_copy.exe.unpacked.exe`) | stolen | verdict |
| --- | --- | --- | --- | --- |
| `exe+0x201460` pool `Allocate` | `rcx=pool, edx=size, r8d=align -> rax` | `44 89 44 24 18 55 56 41 55 41 56 41 57 48 83 EC 10` | 17 B (7 instructions) | **hookable** — 14-byte absolute jump + NOP padding |
| `exe+0x1B2C0` node pop **entry** | `rcx=S, rax=node` | `83 39 00 7F 03 33 C0 C3` (`cmp` / **`jg +3`** / `xor eax,eax` / `ret`) | — | **REFUSED**: the `7F 03` `jg rel8` at offset +3 is a relative branch and cannot be relocated; the function also has **no `.pdata` entry** (leaf), so nothing anchors it. `hook::Probe`/`hook::Install` reject it, and `hooktest` Test7 asserts that. |
| `exe+0x1B2C8` = `0x1B2C0+8` — **the `jg` target** | `rcx=S, rax=node` | `48 8D 91 18 02 00 00 4C 8B 42 08 4C 3B C2` | 14 B (3 instructions) | **installed instead** — exactly the 14-byte absolute-jump form |
| `exe+0x1B7B00` per-frame world tick | `rcx=world` (`mov rdi,rcx` at `+0x16`); returns `al` | `40 56 48 81 EC 80 00 00 00 8B 81 90 87 0A 00` | 15 B (3 instructions) | **hookable** |

**Why the `0x1B2C8` fallback is equivalent for this probe.** `0x1B2C0` is
`cmp [rcx],0 / jg 0x1B2C8 / xor eax,eax / ret`: every call with capacity > 0 is routed
by that `jg` into `0x1B2C8`, and both remaining return paths of the function go through
it (L4 pop → `ret 0x1B2F3`; L2 fallback pop → `ret 0x1B325`). The only paths that skip
it (`capacity <= 0` at `0x1B2C5`, L2 empty at `0x1B303`) return **NULL**, i.e. no node.
The detour executes the stolen prologue through the trampoline, lets the original body
finish (it returns the node in `rax`), then reads `_ReturnAddress()` — which is the
return address of the `0x1B2C0` caller, i.e. the spawn site. `hooktest` Test7 pins all
of this offline, including the refusal reason, so a game update is caught before a run.

### The six spawn call sites (what a captured caller RVA must be)

| call instruction | return address (what the probe logs) | containing function |
| --- | --- | --- |
| `0x125699` | `exe+0x12569E` | `exe+0x1254A2` (size 0x3D7) |
| `0x12B888` | `exe+0x12B88D` | `exe+0x12B631` (size 0x795) |
| `0x13B0A6` | `exe+0x13B0AB` | `exe+0x13B09B` (size 0x11A) |
| `0x13D6FE` | `exe+0x13D703` | `exe+0x13D665` (size 0x1E2) |
| `0x14131D` | `exe+0x141322` | `exe+0x1412B4` (size 0x2B4) |
| `0x1C70C3` | `exe+0x1C70C8` | `exe+0x1C7080` (size 0x16B) |

The probe logs the raw return RVA and tags the containing function
(`spawn@0x1254A2`, …) when it matches one of these ranges.

---

## 2. What one run collects

All lines start with `PROBE-A:` and go through `L().Key` (main log + key log; the key
log cap was raised to 512 KiB so a full run fits without rotation). The detours
themselves never log or allocate — they publish into fixed lock-free arrays that
`Patcher::Tick` drains.

| family | when | content |
| --- | --- | --- |
| `HOOK … installed` | once per target, deferred | target RVA, stolen bytes, jump form; a 60 s retry covers the SteamStub-decrypted `.text` |
| `ALLOC caller=… size=…` | per filtered allocation | caller RVA `0xC767D` (WeaponModel, `size = 0x48 + payloadSize`) or `0x16E14B` (mission singleton, `0xA8940`) |
| `WM-ALLOC VERIFIED` | ~0.2 s after each WeaponModel alloc | re-reads the first qword and confirms `== exeBase+0x2AE790`, logs `payloadSize = size - 0x48` |
| `POP node=… caller=…` | per pool-node pop (cap 64 logged / 256 stored) | node pointer, spawn-site return address, site tag |
| `PARAM object #n …` + `rec …` | once per unique `node+0x28` pointer (max 8) | 0x200-byte raw hex/float dump with value markers, plus the explicit flat-record check (below) |
| `SAMPLE r/12 node=… changed=…` | 12 × 200 ms per captured node (max 8 nodes) | which u32 offsets `0x00..0x6C` changed vs the previous sample (hex + float), explicit `v20/v30/v40` vectors, `+0x4C`, `+0x60`, `+0x64`, `+0x66/+0x67`, `+0x68`, `+0x6B` |
| `S=… L1/L2/free/L5 …` | each sample | singleton `exe+0x3219A0` list heads/tails (`+0x20/+0x28`, `+0x98/+0xA0`, `+0x290/+0x298`), counts `+0x90/+0x210/+0x288/+0x300`, cached `+0x318` |
| `GLIST walk n/30` + `gnode …` | 1/s, max 30 non-empty walks | global time-sorted list `exe+0x33EDB0`: next `+0x00`, `+0x20` u16, `+0x28` param ptr, `+0x60` int, `+0x6B` state, floats `+0x2C..+0x4C` |
| `WORLD t/120 …` + `poolL2` + `movers` + `obj i=…` | every 250 ms while something is flying, max 120 snapshots | frame counters (`probe` and engine `exe+0x33ED94`), player object index (`exe+0x3409F2`), object count/array/perObject base, live pool list L2 (`S+0x98`), moving objects (part-0 `+0x258` pos, `+0x268` heading, `+0x1F0` alive, object `+0x110/+0x118/+0x120/+0x15D`), per-object sim cache `+0x70..+0x8C`, delta since the previous snapshot |
| `WM scan n/5` + `WM object #n` | after the mission singleton alloc (+2 s, retry 10 s ×5) | vtable scan for `exeBase+0x2AE790` (cap 64 hits), full `obj+0x48..0x300` dump of up to 8 distinct objects |
| `R3 …` | one bounded pass | one combined memory pass collecting 1600/2000/3000/40/80/1000/90 (f32, i32, u16, u8 encodings), then the top-10 anchor→companion distances within ±0x2000 |
| `heartbeat …` | 5 s | counters (hook state, alloc/pop events, nodes, samples, scans, overflows) |

### The flat `.wpb` record check (the decisive line)

`node+0x28` is the pointer **P** to the runtime parameter block
(`docs/WEAPON_MODEL_LAYOUT.md`). For every unique P the probe logs the actual values at
`P-0x10` (`data_off`), `P+0x10` (scope 1600), `P+0x14` (range 3000/2000), `P+0x24`
(timeStep 4.0), `P+0x2C` (lead 40/80/28/10), `P+0x44` (speed 1000, u32), `P+0x48`
(type 2), `P+0x49` (attack), `P+0x4A` (startAng 90/60/30) — tolerantly matched, with the
variant named (`mis`/`misf`/`misf2`/`mis_old`) — and then

```
PROBE-A:   rec VALIDATED (flat .wpb record, variant=mis): …
```

When that fires, the two discovery passes (WeaponModel vtable scan, R3 tally) are
skipped for the rest of the run: they only exist to *find* this layout.

---

## 3. How to run (one sortie)

```
1. cd ACZPropNav && ./build.sh test        # must be all green
2. copy the freshly built ACZPropNav.dll into ACZero\, then edit
   ACZero\ACZPropNav.ini [runtime] to Probe=1 / RingSurvey=0 /
   GuidanceOverride=0 / DryRun=0 and set LogLevel=2 (else the PROBE-A
   lines are not written). Back the INI up first.
3. start the game, enter a FREE MISSION
4. lock an AIRBORNE target and fire ~10 PLAIN MSL (no QAAM/SAAM/XLAA/XMAA/XAGM)
5. once, while a missile is in flight, change the lock to another target
6. fly straight for ~20 s, then exit the game normally
7. send ACZero\ACZPropNav.key.log (plus ACZPropNav.log if the key log rotated early)
8. edit the INI back to GuidanceOverride=1 / Probe=0 / RingSurvey=0 to restore
   proportional navigation
```

> Steps 2 and 8 used to be `probe_model.cmd` and `enable_pn.cmd`; **both scripts have
> since been removed**, and the INI is now edited by hand. Because you are editing it
> by hand: keep UTF-8 **with BOM** and don't let the editor rewrite line endings.

Expected first lines within a few seconds: the three `HOOK … installed` lines. They are
deferred until SteamStub has decrypted `.text` (retry every 200 ms, max 60 s) — the
`waiting for the SteamStub-decrypted .text` line is normal, not an error.

---

## 4. What each observation decides

* **`rec VALIDATED`** ⇒ the `.wpb` parameter block is a plain struct at `P`, so the
  mod can read/replace `scope/range/lead/speed/startAng` on a live missile without any
  serialization work. If instead `rec NOT validated` with `data_off`/scope/type/speed
  values, those values say what the runtime really looks like (the R3 tallies then
  suggest the actual stride).
* **`POP … spawn@…` + `PARAM`** ⇒ which spawn path produced a node and which parameter
  record it points at (player vs wingman/enemy missile).
* **`WORLD` snapshots** ⇒ exactly one object's `p0pos` starts at the launch node's
  position and advances every snapshot: that object/part is the flying missile. Its
  per-snapshot delta is the velocity in m/frame; whether `p0dir` follows a mid-flight
  lock change says whether the aim comes from the part transform or from the
  fire-state (`+0x1650/+0x1664`).
* **`SAMPLE`** ⇒ whether the pool node itself moves; per `docs/MSL_ADVANCE.md` it
  should not (it is a spawn/launch record, not the flying missile).
* **`GLIST`** ⇒ the dispatcher's own time-sorted node list, aligned in time with the
  `WORLD` snapshots (same `+0x60` timestamps and `+0x6B` states).

---

## 5. Deviations from the original Run A spec (and why)

1. **Node-pop hook point is `0x1B2C8`, not `0x1B2C0`** — `0x1B2C0` cannot be hooked by
   this engine (relative `jg` at +3, no `.pdata`). See the table above; the data
   collected is identical (all non-null returns flow through `0x1B2C8`).
2. **The world-tick hook (`0x1B7B00`) and its snapshots were added** after
   `docs/MSL_ADVANCE.md` proved the pool node is not the flying missile. It is the
   cheapest per-frame anchor for the real part transform.
3. **R3 is one combined pass, not one pass per value.** It collects the addresses of
   all 11 value encodings in a single sweep (bounded to ~2 GB, non-executable data
   regions only) and then compares anchors to companions within ±0x2000. The resulting
   deltas are exactly the R3 experiment; the per-value loop would have re-read every
   region up to 11 times.
4. **Both heavy passes skip executable regions and this module's own image.** Code
   pages contain byte values (e.g. `0x5A` = 90) that would drown the u8/u16 hit sets;
   our own static tables contain the very float patterns being searched for.
5. **`WeaponModel` scan and R3 are skipped once `rec VALIDATED` fires** — they are only
   locators for the layout the PARAM check proves directly.
6. **The key log cap was raised 250 → 512 KiB** (`src/core/log.cpp`) so a full run's
   intentional output (≈250 KiB) does not trigger the "keep recent half" rotation that
   would discard the early hook/alloc events.
7. **World snapshots are capped at 120 × 250 ms** and only consume a slot when
   something is actually flying/changing; the object scan covers up to 64 objects, the
   detail line is logged for the fastest mover every snapshot and for every mover on the
   first snapshot.
8. **The legacy one-shot `Patcher::Probe()` (dispatcher hook + heap scans) is skipped**
   when the Run A probe is active — it would add a fourth hook and more scan noise.

---

## 6. Risks / crash-safety notes

* Everything read from the game goes through `mem::Read*` / `mem::ReadBytes` (SEH-guarded
  by the mem engine); every pointer is validated with `mem::IsReadable` before a read.
* Detour bodies are allocation-free and lock-free; they publish into fixed arrays with
  `Interlocked` and are drained from the mod thread (no logging inside a detour, so a
  detour can never deadlock on the log critical section).
* The allocations hooks only *record* values; nothing is written to game memory. The
  three patches are restored in `ProbeModel::Shutdown` (`hook::Remove`).
* The pool `Allocate` detour is on a game-wide hot path: its fast path is two RVA
  compares (the caller is filtered before anything else happens).
* If a prologue does not match the expected bytes (a different game build), the probe
  logs `waiting for the SteamStub-decrypted .text`/a refusal and keeps running
  read-only; it never patches a prologue it has not byte-verified.
* A missile must actually be fired for the node/PARAM/sample data to appear; the probe
  logs a reminder at +90 s if nothing was captured. The world snapshots need the
  `0x1B7B00` hook to have installed (same deferred window as the other two).
