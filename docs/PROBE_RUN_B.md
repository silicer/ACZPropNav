# PROBE_RUN_B — hardware read-watch on the weapon-parameter object (design)

Purpose: after Run A (`docs/PROBE_RUN_A.md`) has located the live weapon-parameter object
(`param = *(void**)(poolNode+0x28)` — see `docs/WEAPON_DISPATCH.md`) and its internal
layout, arm an **x86-64 hardware data breakpoint** on the 4-byte field(s) we care about
(`lead`, and the `+0x44` int used by the launch formula) and log every instruction that
reads them. This directly answers the question "who consumes `lead` during flight?" — the
one the whole PN port depends on — without a debugger and without touching game memory.

## Why hardware breakpoints (and not the two things that already crashed)

* `PAGE_GUARD` write-watch crashed the game twice (`docs/VERIFICATION.md`); do not reuse.
* `PAGE_GUARD` needs a page copy and changes protection — no.
* DR breakpoints are pure CPU state: no page protection change, no memory write, no
  copying. The process already runs a VEH (`src/core/patcher.cpp` `AczGuardVeh`) so the
  exception plumbing is proven in this binary.

## Mechanism

```text
DR0 = watched 4-byte address
DR7 = L0 (bit 0) | R/W0 = 11b (read/write) | LEN0 = 11b (4 bytes)     ; bits 16..19
      no GE/LE (bits 8/9 = 0), G0 = 0 (local enable only)
DR6 = cleared in the VEH before continuing
```

The breakpoint must be armed **per thread** (debug registers are thread state):

```text
CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)
for each thread of GetCurrentProcessId() that is not our own:
    OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT)
    SuspendThread; GetThreadContext(CONTEXT_DEBUG_REGISTERS|CONTEXT_CONTROL)
    ctx.Dr0..Dr3 / ctx.Dr7 = our slots; SetThreadContext; ResumeThread
```

New threads are missed; re-run the enumeration every ~500 ms while armed (the game
threads are few and stable). Disarm = same enumeration with Dr7 = 0.

## Exception handling

`AddVectoredExceptionHandler(1, handler)` (own handler, not the guard one):

```text
if (!armed) return CONTINUE_SEARCH
if (code != EXCEPTION_SINGLE_STEP /*0x80000004*/) return CONTINUE_SEARCH
slot = lowest set bit of ContextRecord->Dr6 & 0xF ; if none return CONTINUE_SEARCH
record { hittime, slot, Rip, Dr6, Rax, Rcx, Rdx, R8, R9, Rsp } into a fixed array
    (InterlockedIncrement index, cap 500; overflow counter if full)
ContextRecord->Dr6 = 0
return EXCEPTION_CONTINUE_EXECUTION
```

Rules for the handler: **no logging, no allocation, no locks, no formatted I/O** — only
the fixed-array record (a faulting I/O inside an exception handler is how you turn a
probe into a crash). All formatting happens in `Tick`.

Note on `Rip` (corrected after the offline test, see the As-implemented section): a data
breakpoint on this Windows build is reported as a **fault**, so `Rip` is the instruction that
performed the read. Setting RF and continuing re-faults forever. The probe therefore also logs the 16
bytes before `Rip` (`mem::ReadBytes(Rip-16, 16)`) in `Tick`; the static analyst decodes
which instruction actually touched the address. Multiple hits from the same reader give
the same `Rip`, so the reader set is unambiguous even before decoding.

## What to arm (chosen after Run A)

Up to 4 slots, priority order:

1. `param + leadOffset` — the `lead` value member (Run A gives the exact offset).
2. `param + 0x44` — the int read by `FUN_14013D180`'s speed formula (proven consumer).
3. `param + scopeOffset` — seeker acquisition (HUD also reads it; expect noise, use as
   a control: HUD reads happen every frame in the menu too).
4. `param + startAngOffset` or `rangeOffset`.

Arm **only after a missile is in flight** (Run A / a marker file / a delay), otherwise the
HUD/menu readers dominate. Cap: disarm automatically after 20 s or 500 hits, whichever
first; `ACZPropNav.key.log` gets one `PROBE-B: reader RVA … count=…` line per distinct RIP
with the raw context of the first hit.

## Expected outcomes

* **A per-frame reader that is not `FUN_14013D180`** → that is the MSL guidance/interceptor
  code path; decompile it (Ghidra), find the target-vector source it uses, then PN can be
  computed exactly like the `WeaponEx` path and written where that function commits the
  transform.
* **Only `FUN_14013D180` (launch) reads `lead`** → `lead` is a launch-time aim offset only.
  Then the PN implementation has to be a real code hook on the flight update (Run A /
  `docs/MSL_ADVANCE.md` path) and the INI `HomingForesightAmount` is implemented as a
  lead-bias at launch plus a per-frame correction, not as a pure data patch.
* **No hits at all** → the field is not the runtime `lead` (wrong object/offset); Run A
  dump has to be re-read before arming again.

## Offline acceptance test (before the run)

Write a small test tool `tools/hwwatchtest` (same build.sh style) that:

1. allocates a 16-byte buffer and sets a hardware read/write breakpoint on it on its own
   thread via the same `ArmThreads/Disarm` code (single-threaded variant),
2. reads the buffer through a `volatile` pointer,
3. asserts the VEH fired exactly once, that the recorded `Rip` is inside the reading
   function and `Dr6 & 1` was set,
4. repeats with a write, and with two slots, then disarms and asserts no further hits.

This proves the DR programming + VEH round-trip on this Windows build without the game.
Only then ship it behind a new INI key (`HwWatch = 1`) and run it in-game once.

## Risk / abort conditions

* SteamStub anti-debug in `acz.exe` (the stub already unpacked by the time our DLL runs;
  no evidence it inspects DR registers — the mod has lived in-process for many sessions).
* A #DB in a thread we did not arm cannot happen (per-thread).
* If the game crashes/hangs during the armed window, log lines already written contain
  every hit before the crash; keep the window short (20 s).

---

## As implemented (shipped code, 2026-10-26)

Files: `src/core/probe_watch.h` / `src/core/probe_watch.cpp` (new), wired into
`Patcher::Start/Tick/Shutdown`; keys `[runtime] HwWatch` (default 0) and
`HwWatchSeconds` (default 25, clamped 1..600) parsed in `src/core/config.cpp`;
`tools/hwwatchtest` is now part of
`./build.sh test` (21 checks, green) and is the offline proof of the mechanism.
*(The `probe_watch.cmd` / `probe_watch.ps1` toggle scripts that used to set those keys and
copy the DLL have since been removed; set `HwWatch = 1` / `HwWatchSeconds = 60` by hand in
`ACZero\ACZPropNav.ini` and add `LogLevel = 2` so the hits are actually logged.)*

### Corrections proven by `tools/hwwatchtest`

* A 4-byte read/write watchpoint is delivered as `EXCEPTION_SINGLE_STEP` with the slot flagged
  in `Dr6`, and the saved `Rip` **is the faulting instruction** (fault, not trap).
* A data breakpoint is a **fault**: setting `RF` (EFLAGS bit 8) and returning
  `EXCEPTION_CONTINUE_EXECUTION` re-executes and re-faults forever; Windows then terminates
  the process (`0x80000004`). The shipping scheme is therefore one-shot: the VEH records the
  hit, clears `Dr0..Dr3`/`Dr6`/`Dr7` in the returning context and continues; the tick re-arms
  every ~200 ms.

### What it actually does

1. Defers until the mission is live (retry every 200 ms; gives up at **180 s** — menus can take
   longer than the 60 s used for the SteamStub `.text` wait): `MC = *(u64*)(base+0x3409F8)`
   (fallback `*(S+0x428)`), `objArray = *(MC+0x697B8)`, `count = *(u8*)(MC+0xA87E4)` (1..64),
   `simBase = *(MC+0xA8248)`.
2. Locates `P`: `playerIdx = *(u8*)(base+0x3409F2)` → `objArray[playerIdx]` → `+0x110` part0 →
   `+0x1B8` fire/WC → `+0x1FB8` = P; validates `*(u32*)(P-0x10)==0x10`, scope 800..4000,
   range 500..5000, lead 0..400, speed 100..3000. On failure it scans `objArray[0..min(count,32)]`
   the same way and logs which object index was used. If the whole chain fails it logs
   `RESOLVE FAILED raw: …` and stops gracefully (no guessing).
3. Logs `CHAIN …`, the validated `P` values and a `P-0x10..P+0x50` hexdump, then samples
   `*(u32*)(base+0x33ED94)` at start and +2 s and logs `DISPATCHER COUNTER ADVANCES` or
   `… FROZEN` (the dispatcher's only frame-counter writer).
4. Runs the one-shot **vtable presence scan** (7 controller vtables, 512-hit cap, own module
   skipped, 8-byte aligned; FireCtrl objects get mask + live-slot dumps) *before* arming, so it
   does not disturb the measurement.
5. Arms the 4 watchpoints over every thread, then re-arms every 200 ms and drains the lock-free
   hit ring. Per hit: `RAW hit #…` (slot, Dr6, Rip, rax/rcx/rdx/r8/r9/rsp, tid); per distinct
   Rip: `HIT … caller0/caller1` (two stack return-address hints) and a final `RIPSUM … count=…`.
   Rip is resolved to `exe+RVA` only when inside the main module.
6. Every 250 ms: `MOV t=… obj=… team=… flags=… pos=… dpos=… head=… engineFrame=…` for the
   fastest-moving world object/part, and `POOL …` for pool nodes whose `+0x28 == P` (the
   plain-MSL launch records, sampled each tick, max 8).
7. When the allocation hook is available (i.e. Run A is off) it also captures the live
   `WeaponModel` allocation (caller `exe+0xC767D`) and dumps `obj+0x48..` once per object
   (cap 0x1000 bytes, row budget 900) with f32 markers 1600/3000/2000/400/40/80/28/10/90 and
   u32 1000.
8. Ends after `HwWatchSeconds` (the removed `probe_watch` scripts set 60 s; the INI default
   is 25 s) or 400 hits: disarms all threads, clears the
   armed flag, logs per-Rip totals + a heartbeat-like summary, then `Shutdown()` disarms again
   and removes the VEH.

### Deviations from the design above

* `Rip` is the faulting instruction (fault, not trap) — see the corrected note.
* The window starts when P validates (mission data live), not "after a missile is in flight";
  the plain-MSL launch records are visible immediately (`POOL` lines), and 60 s lets the user
  fire several missiles.
* Resolve retry cap is 180 s (the design said 60 s; the mission can only be entered from the
  menu).
* Re-enumeration cadence is 200 ms (design said ~500 ms) so post-hit threads are re-armed
  quickly; all four slots are used from the start (lead at `P+0x2C`, `P+0x48`, range `P+0x14`,
  speed `P+0x44`).
