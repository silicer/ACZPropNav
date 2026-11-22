# ACZPropNav — architecture & module contracts

Mod for **ACE COMBAT ZERO: THE BELKAN WAR (Steam, AppID 5370600)**, Windows x86-64.
Goal: replace the vanilla tail-chase missile homing with **proportional navigation
(PN)** and expose the *same configuration surface and semantics* as the AC8 mod
`AC8PropNav` (`AC8PropNav.ini` / `AC8PropNav.dll` found in
`steamapps/common/ACE COMBAT 8/Game/Binaries/Win64`).

## Why a DLL and not a data edit

* `ACZero/acz.exe` is Steam-DRM wrapped (`.bind` stub decrypts `.text` at runtime),
  so no static patching of the code is possible offline.
* `ACZero/*.arc` are LZ4-packed asset archives (see `docs/ARC.md`), they carry data,
  not the guidance loop.
* A config file that the *game* reads (`ACZPropNav.ini`) can only work through an
  injected DLL, which is exactly how `AC8PropNav.dll` works on the UE5 title.

So: a DLL that is loaded through a DLL-proxy, reads `ACZPropNav.ini`, and applies the
settings by steering the missiles in the game's own per-frame update.

## Layering

```
                       ACZero/acz.exe
                              | imports
                       XINPUT1_4.dll        <- loader/proxy, forwards to System32, then
                              |                 LoadLibrary("ACZPropNav.dll")
                       ACZPropNav.dll
   +--------------------------+---------------------------------------------+
   | src/core  config.cpp     | parse ACZPropNav.ini (AC8-compatible schema) |
   | src/core  log.cpp        | capped main log + key.log                    |
   | src/core  patcher.cpp    | integration: the two hooks, probes, tick     |
   | src/core  mod.cpp        | bootstrap thread, safety, tick loop          |
   +--------------------------+---------------------------------------------+
   | src/pn    msl_pn.cpp     | plain-MSL post-update hook + slot scan + log |
   | src/pn    msl_steer.h    | true λ̇ PN math (pure, `pnsim`-tested)        |
   | src/pn    pn.cpp         | pursuit / PN / blend / rate limit / FOV      |
   | src/mem   mem.cpp        | fault-safe read/write, region scan, float runs |
   | src/mem   discovery.cpp  | param-table + entity correlator (opt-in)     |
   | src/hook  hook.cpp       | conservative x64 inline hook engine          |
   +-----------------------------------------------------------------------+
```

## Module map

| Path | Responsibility |
| --- | --- |
| `src/common/`, `src/core/config.*`, `src/core/log.*` | shared types/vector math, INI parser, logging |
| `src/core/mod.cpp` | bootstrap thread, DllMain / `AczAddonInit`, tick loop, shutdown |
| `src/core/patcher.cpp` | integration: hook arming, discovery, probes, per-frame telemetry |
| `src/core/probe_*.{h,cpp}` | the two read-only diagnostic probes (`Probe`, `HwWatch`) |
| `src/pn/msl_pn.cpp` + `msl_steer.h` | plain-MSL hook, slot scan, true λ̇ PN math |
| `src/pn/pn.cpp` + `pn.h` | pursuit / lead-point PN / blend / rate limit / FOV (also the `pnsim` model) |
| `src/mem/*` | fault-safe read/write, region scan, discovery correlator |
| `src/hook/hook.cpp` | conservative x64 inline hook engine (refuses anything it cannot relocate) |
| `loader/` | `XINPUT1_4.dll` and `VERSION.dll` proxies |
| `tools/` | offline tests, `.arc`/`.wpb` tooling, RE scripts |
| `docs/` | this documentation set (see `docs/INDEX.md`) |

Every test lives in its own `tools/<name>/` directory and is picked up by `build.sh`
(which wildcards `src/**/*.cpp`). See `AGENTS.md` for conventions.

## Hard rules (safety, because we inject into a live game)

1. **No unvalidated writes.** Each missile slot must prove it really is a missile slot
   (live mask bit + the object at `ctrl` is a weapon controller) before the PN hooks write
   it; the static-parameter path only writes rows a locator proved (or the user forced with
   `ForceOffsets=1`).
2. **Every dereference is fault-safe, and nothing here is exception-based.** Reads and
   writes go through `ReadProcessMemory`/`WriteProcessMemory`, which range-check in the
   kernel and return FALSE instead of raising in our thread.
   * There is **no `__try/__except`**: the MinGW-w64 toolchain does not emit a Microsoft
     exception handler for it (no `__C_specific_handler`, no `.xdata` funclet), so a real
     access violation would propagate as an unhandled AV. A safety construct that
     silently does nothing is worse than a documented absence.
   * There is **no VEH handler and no `setjmp`/`longjmp` either**. An earlier design used
     exactly that around a raw `memcpy`; it died on the first bad read, because `longjmp`
     out of a vectored exception handler is not a supported x64 unwinding path. It was
     also pointless as a "partial range" fallback: it returned FALSE on the same ranges
     the kernel refuses, so all it added was a real hardware fault thrown inside the
     game's thread. See `src/mem/mem.cpp` for the full history.
   * No exception may ever propagate into the game.
3. **No exceptions, no CRT heap churn in hot paths.** Build with `-fno-exceptions`.
   Never log or allocate from inside a detour.
4. **Dry-run by default when unsure.** If `RequireVerifiedOffsets=1` (default) and
   nothing verifies, log loudly and change nothing.
5. **Uninstallable.** Uninstalling means deleting `ACZPropNav.dll` + `XINPUT1_4.dll`
   from `ACZero\` (the INI and the logs are user data, left behind). Nothing else is
   touched. Never modify `acz.exe` or `*.arc`.
6. **Logs are the calibration interface.** Follow AC8PropNav's style: `[CORE]` tags,
   `ACZPropNav.log` (500 KiB cap) and `ACZPropNav.key.log` (512 KiB cap, keep recent
   half). Users can paste the key log to continue calibration — the diagnostic detail
   channels only write when `[runtime] LogLevel = 2` (blank/absent = 1 = quiet:
   startup, hook status, warnings/errors only; `0` = nothing is logged at all).

## Interfaces

* `src/core/config.h` — INI model, `Config::LoadFromFile`, `Config::Resolve(side,type)`.
* `src/pn/pn.h` — `ComputeGuidance(input, params) -> GuidanceResult` (+ LOS helpers).
* `src/mem/mem.h` — fault-safe primitives & scanners.
* `src/mem/discovery.h` — finds missile parameter tables / entities (contract below).
* `src/hook/hook.h` — `hook::Install/Remove/Probe`.

## Discovery contract (`src/mem/discovery.h`)

*Opt-in machinery for the static-parameter path; the PN hooks do not use it.* It exists
because the game is Steam-packed and undocumented, so if a future build ever exposes the
records again the mod can find them by value instead of by hard-coded offsets:

1. **Anchor search** — for each `KnownValue` in `DiscoveryOptions::known` (a
   per-missile-type list of `{property, value, tolerance}` derived from
   `docs/VANILLA.md`), find every 4-byte-aligned float in writable memory that
   matches.  Cheap because we scan once and bucket by value.
2. **Relative-offset correlation** — for every anchor hit, look inside a window
   `[-0x40, +0x80]` for the *other* properties of the same model at a consistent
   relative offset.  A layout hypothesis `(offsetA, offsetB, offsetC)` that repeats
   across many hits is almost certainly the parameter struct.  Score = number of
   hits × number of matched properties.
3. **Cross-model validation** — repeat with a second missile model; the hypothesis
   must also match *its* values (e.g. QAAM FOV 120 vs MSL 75) at the same offsets.
   Only layouts that survive both are reported as `verified`.
4. Everything not verified goes to the log with its score so a user can hand the log
   back for calibration.

`DiscoveryOptions::known` is filled from `docs/VANILLA.md` (ACZ vanilla values) by
`patcher.cpp`; the INI can override it in `[runtime]`/`[vanilla]` (see `docs/CONFIG.md`).

## Guidance override

What ships are **two post-update hooks** that run the PN math on the game's own state,
each independent of the value-correlation path above (so they work even when nothing was
ever "discovered"):

| path | hook | written | math | authority |
| --- | --- | --- | --- | --- |
| plain MSL | `FUN_14013ECA0` @ `RVA 0x13ECA0`, after the vanilla body | missile **position** (`slot+0x00..0x08`), and optionally the slot's attitude | `src/pn/msl_steer.h::ComputePn` — true `ω = N·λ̇` rate law | `docs/MSL_PN.md` |
| special weapons | steering integrator @ `RVA 0x90510`, after the vanilla body | missile **velocity** (`slot+0x20..0x2C`), same speed magnitude | `src/pn/pn.cpp::ComputeGuidance` — lead-point PN blended with pursuit | `docs/MSL_PN.md`, `docs/PN.md` |

Both take `HomingForesightAmount` as the pursuit↔PN blend, rate-limit the commanded turn by
`MaxRotationAngle · (1/gameFps)` and gate on the seeker cone; they differ only in how the PN
demand is expressed (a rate, vs. an aim point) and therefore in which write they can make.
Neither hook logs or allocates: they publish into fixed arrays that `Tick()` drains on the
worker thread. Addresses, entry bytes and the full slot/field maps are in
`docs/HANDOFF.md` §3.

One consequence worth knowing: the engine does not tell the mod which weapon a
special-weapon slot belongs to, so that whole path is driven by the **`[player] msl`**
entry of the INI (all homing special weapons share it). The plain-MSL path *does* separate
`[player]` from `[enemy]` per slot.

Optional static adapter (**ParamPatch**, no hooks, opt-in via the INI's `[vanilla]`
section): writes the four static properties (`MaxRotationAngle`, `MaxHomingAngle`,
`LifeTime`, `LockonRange`) into the verified parameter rows. Safe and read-back verified —
but see the warning under "Discovery order": on the shipping build the records are
re-serialised, so this path does not find them and is off by default.

The older "AimPoint / lead point" description — steer toward
`targetPos + targetVel·tGo·lead`, `tGo = range/closingSpeed` — is what the special-weapon
path still does (`pn.cpp::ProportionalNavigationDirection`), and it is what `pnsim`'s
engagement sweeps in `docs/PN.md` measure. The plain-MSL path moved to the λ̇ rate form in
`msl_steer.h` because writing a *position* lets it command the turn rate directly instead
of expressing it as an aim point.

## Tests (all runnable offline, `./build.sh test`)

* `initest` — INI parser vs. the real `AC8PropNav.ini` (11 player + 1 enemy models,
  blank = vanilla, legacy `msl = 0.8`, longest-key-first resolution, clamping).
* `pnsim` — guidance math + engagement sim: tail-chase vs PN vs blend over many
  geometries; prints miss distances, hit rates, FOV losses. **69 assertions**; the only
  suite that also runs natively on a non-Windows host.
* `memtest` — synthesises a fake memory image (parameter table + entities) and
  checks the correlator finds exactly it, plus fault-safe read/write behaviour
  (including cross-page protection restore).
* `hooktest` — hooks functions in-process, validates trampoline + refusal rules, and
  pins the entry bytes of every hook target the mod installs.
* `hwwatchtest` — the hardware-read-watch mechanism (one-shot debug registers + VEH,
  including cross-thread arming).
* `smoke` — loads the real `ACZPropNav.dll` into a synthetic process, calls its
  exported entry point and asserts that the boot sequence, the discovery verdict, the
  log files and the tick loop all work without crashing.

## Final implementation notes (what actually ships)

### Distribution layout
One zip whose contents extract **directly** into `ACZero\` (the directory holding
`acz.exe`). No `build/` subfolder, no installer script, no diagnostic scripts:

```
ACZero\
  ACZPropNav.dll     the mod
  XINPUT1_4.dll      loader/proxy (forwards to System32\XInput1_4.dll)
  ACZPropNav.ini     configuration (UTF-8 with BOM, Chinese comments)
  README.md  README.en.md  LICENSE  RELEASE_NOTES.md
```

Install = copy/unzip those files into `ACZero\`. Uninstall = delete `ACZPropNav.dll` +
`XINPUT1_4.dll`. Behaviour is changed only through `ACZPropNav.ini`, which the user edits
by hand (`[runtime]` switches such as `GuidanceOverride`, `Probe`, `HwWatch`, `LogLevel`).
`VERSION.dll` (the alternative proxy for the community modloader) can still be built from
`loader/`, but it is not part of the release.

### Two entry points
* `DllMain` → worker thread: the self-contained path, used with our own proxy
  (`XINPUT1_4.dll`, alternative `VERSION.dll`).
* `AczAddonInit(void* hostCtx)` → the community `version.dll` preloader path
  (`ACZero\mods\ACZPropNav.dll`). Host context layout: `+0x08` game dir, `+0x10`
  `HMODULE` of acz.exe, `+0x28` `printf` logger. Both paths converge on the same
  idempotent `StartOnce()`; the INI is looked up in `mods\ACZPropNav.ini`, then next to
  the DLL, then in the game dir, in that order.

### Discovery order (in `patcher.cpp`)
1. **Exact-record scan** — only when the INI has a `[vanilla]` section. The shipping
   missile records are 96-byte structs loaded verbatim from `data/mis/mpl/*.wpb`;
   `src/core/vanilla_records.h` (generated by `tools/gen_vanilla_header.py` from the
   extracted shipping archive) embeds their bytes. We scan writable, non-image memory for
   `explPers=1.0 + scope=1600`, validate the record (`data_off==0x10`, `type==2`, sane
   `lead`/`speed`), classify it against the three shipping missiles (msl/misf/misf2) and
   expose the documented field offsets (`lead +0x3C`, `startAng +0x5A`, `range +0x24`,
   `scope +0x20`).
2. **Value-correlation fallback.** If the records were modified by another data mod, the
   generic correlator (`src/mem/discovery.cpp`) searches for the vanilla float tuples,
   infers the layout from repeated relative offsets and cross-validates it against a
   second missile model. Only "verified" rows are ever written.

> **Both stages are opt-in and off by default.** Reverse engineering proved the shipping
> build re-serialises the `.wpb` records into engine structs (every field gets its own
> header, member slot granularity `0x18`), so scanning for the on-disk 96-byte layout is a
> guaranteed — and expensive — miss. The whole static-parameter path only runs when the
> INI declares `[vanilla]` values; otherwise `Patcher::Start` says so in the log and skips
> it. The guidance override does not depend on it.

Hard guards that were added after testing:
* the mod's own image is excluded from every scan — its static tables contain the very
  floats we search for, and without this guard the mod "finds" itself and patches its own
  `.data` (this actually happened in the smoke test and is now covered);
* a staleness guard remembers what we wrote: if a row no longer holds either the vanilla
  value or our own value, the game reloaded its data and we drop all rows and rediscover
  instead of writing into freed memory;
* every write is read-back verified; `DryRun`, `RequireVerifiedOffsets` and the fault-safe
  read/write layer make "nothing found" a no-op instead of a crash.

### Enemy / wingman separation
ACZ stores the player's MSL and the wingmen's MSL as separate records
(`mis.wpb` vs `misf.wpb`/`misf2.wpb`). Records classified as wingman/NPC ordnance take the
`[enemy]` settings (with model-key aliasing `misf → msl` so a normal AC8-style
`[enemy] msl = 0.3` just works). Special weapons have no data record at all (code-side),
so the plain-MSL and integrator PN hooks are the only path for those.

### Possible future work (documented, not implemented)
`acz.exe` ships plaintext MSVC RTTI for ~400 classes (`scripts` for this live in
`tools/re/rtti_scan.py`), including `WeaponModel`, the `WeaponEx*Ctrl` vftables and
`EffRingBuffer<UEFF_MIS_POSDIR>`. Rebuilding the name→vftable map from the live image and
locating missile objects by their vftable would replace the value-correlation stages above
with an exact type match — but since the value path is opt-in and PN does not need it,
this is a robustness improvement, not a missing feature.
