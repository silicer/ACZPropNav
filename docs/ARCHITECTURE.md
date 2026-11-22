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

So: a DLL that is loaded through a DLL-proxy, reads `ACZPropNav.ini`, locates the
live missile parameter structures at runtime and applies the settings.

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
   | src/core  patcher.cpp    | map INI model -> discovered game structures  |
   | src/core  mod.cpp        | bootstrap thread, safety, tick loop          |
   +--------------------------+---------------------------------------------+
   | src/pn    pn.cpp         | pursuit / PN / blend / rate limit / FOV      |
   | src/mem   mem.cpp        | SEH-safe read/write, region scan, float runs |
   | src/mem   discovery.cpp  | param-table + entity correlator              |
   | src/hook  hook.cpp       | conservative x64 inline hook (fallback path) |
   +-----------------------------------------------------------------------+
```

## Module ownership (team)

| Path | Owner | Responsibility |
| --- | --- | --- |
| `src/common/`, `src/core/config.*`, `src/core/log.*` | Lead | shared types, INI parser, logging |
| `src/core/mod.cpp`, `src/core/patcher.cpp`, `ACZPropNav.ini`, docs | Lead | integration, config file, docs |
| `src/pn/*`, `tools/pnsim/*` | pn-core | PN math + offline engagement simulator + verification |
| `src/mem/*`, `tools/memtest/*` | mem-engine | memory primitives, scanners, discovery correlator |
| `src/hook/*`, `loader/*`, `tools/hooktest/*` | loader-hook | inline hook + proxy loader |
| `tools/arc/*`, `docs/ARC.md`, `docs/VANILLA.md` | arc-recon | `.arc` unpacking, vanilla missile parameters |

Everyone keeps test code inside their own `tools/<x>/` directory and their sources
compilable by the root `Makefile` (wildcards pick up `src/**/*.cpp`).

## Hard rules (safety, because we inject into a live game)

1. **No unvalidated writes.** A write to a memory location is only allowed after a
   locator proved the structure (value signature + correlation) or the user forced
   it with `ForceOffsets=1`.
2. **Everything SEH-guarded.** `__try/__except` around every dereference; exceptions
   in our tick loop must never propagate into the game.
3. **No exceptions, no CRT heap churn in hot paths.** Build with `-fno-exceptions`.
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
* `src/mem/mem.h` — SEH-safe primitives & scanners.
* `src/mem/discovery.h` — finds missile parameter tables / entities (contract below).
* `src/hook/hook.h` — `hook::Install/Remove/Probe`.

## Discovery contract (`src/mem/discovery.h`)

The game is Steam-packed and undocumented, so the mod ships a *signature-driven*
locator rather than hard-coded offsets:

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
4. **Entity correlation** (for the guidance override) — from the verified table
   address, find heap objects that hold a pointer to the row; inside those objects
   find the `(missilePos, missileVel, targetPos, targetVel)` vectors by sampling
   two frames and matching `|Δpos| ≈ |vel|·dt` and `Δpos ∥ vel`.
5. Everything not verified goes to the log with its score so a user can hand the log
   back for calibration.

`DiscoveryOptions::known` is filled from `docs/VANILLA.md` (ACZ vanilla values) by
`patcher.cpp`; the INI can override it in `[runtime]`/`[vanilla]` (see README).

## Guidance override

Primary adapter (**AimPoint / lead point**, works once entities are correlated):

* vanilla: missile steers toward `targetPos`
* mod: missile steers toward `targetPos + targetVel * tGo * lead` with
  `tGo = range / closingSpeed`, clamped, blended by `HomingForesightAmount`
* turn command rate-limited by `MaxRotationAngle`, gated by `MaxHomingAngle`.

Fallback adapter (**ParamPatch**, no hooks): writes the four static properties
(`MaxRotationAngle`, `MaxHomingAngle`, `LifeTime`, `LockonRange`) directly into the
verified parameter rows — always active, safe, and useful by itself.

## Tests (all runnable offline, `make test`)

* `initest` — INI parser vs. the real `AC8PropNav.ini` (11 player + 1 enemy models,
  blank = vanilla, legacy `msl = 0.8`, longest-key-first resolution, clamping).
* `pnsim` — engagement sim: tail-chase vs PN vs blend over many geometries; prints
  miss distances, hit rates, FOV losses. Proves the math.
* `memtest` — synthesises a fake memory image (parameter table + entities) and
  checks the correlator finds exactly it, plus SEH-safe read/write behaviour.
* `hooktest` — hooks functions in-process, validates trampoline + refusal rules.
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
  (`ACZero\mods\ACZPropNav.dll`). Host context layout proven in `docs/RE.md`:
  `+0x08` game dir, `+0x10` `HMODULE` of acz.exe, `+0x28` `printf` logger. Both paths
  converge on the same idempotent `StartOnce()`; the INI is looked up in
  `mods\ACZPropNav.ini`, then next to the DLL, then in the game dir, in that order.

### Discovery order (in `patcher.cpp`)
1. **Exact-record scan (primary).** The shipping missile records are 96-byte structs
   loaded verbatim from `data/mis/mpl/*.wpb`; `src/core/vanilla_records.h` (generated by
   `tools/gen_vanilla_header.py` from the extracted shipping archive) embeds their bytes.
   We scan writable, non-image memory for `explPers=1.0 + scope=1600`, validate the record
   (`data_off==0x10`, `type==2`, sane `lead`/`speed`), classify it against the three
   shipping missiles (msl/misf/misf2) and expose the documented field offsets
   (`lead +0x3C`, `startAng +0x5A`, `range +0x24`, `scope +0x20`). This is exact and fast.
2. **Value-correlation fallback.** If the records were modified by another data mod, the
   generic correlator (`src/mem/discovery.cpp`) searches for the vanilla float tuples,
   infers the layout from repeated relative offsets and cross-validates it against a
   second missile model. Only "verified" rows are ever written.

Hard guards that were added after testing:
* the mod's own image is excluded from every scan — its static tables contain the very
  floats we search for, and without this guard the mod "finds" itself and patches its own
  `.data` (this actually happened in the smoke test and is now covered);
* a staleness guard remembers what we wrote: if a row no longer holds either the vanilla
  value or our own value, the game reloaded its data and we drop all rows and rediscover
  instead of writing into freed memory;
* every write is read-back verified; `DryRun`, `RequireVerifiedOffsets` and the SEH-guarded
  read/write layer make "nothing found" a no-op instead of a crash.

### Enemy / wingman separation
ACZ stores the player's MSL and the wingmen's MSL as separate records
(`mis.wpb` vs `misf.wpb`/`misf2.wpb`). Records classified as wingman/NPC ordnance take the
`[enemy]` settings (with model-key aliasing `misf → msl` so a normal AC8-style
`[enemy] msl = 0.3` just works). Special weapons have no data record at all (code-side);
the mod logs which configured models could not be served and that the PN override is the
only path for those.

### Roadmap (documented, not implemented)
`docs/RE.md` shows that `acz.exe` ships plaintext MSVC RTTI for ~400 classes, including
`WeaponModel` and `WeaponEx*Ctrl` vftables and `EffRingBuffer<UEFF_MIS_POSDIR>`. The next
step for true per-frame PN without any manual calibration is to rebuild the
name→vftable map from the live image and locate missile objects by their vftable, which
replaces stage 2/3 of the entity correlator with an exact type match.
