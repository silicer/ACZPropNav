# ACZPropNav — Proportional-Navigation missiles for ACE COMBAT ZERO: THE BELKAN WAR

[中文说明](README.md) · [Calibration guide](docs/CALIBRATION.md) · [Vanilla values](docs/VANILLA.md) · [Architecture](docs/ARCHITECTURE.md)

Replaces the vanilla tail-chase missile homing of the Steam port of *Ace Combat Zero*
with real **proportional navigation (PN)**, configured through an INI that is a strict
superset of the AC8 mod `AC8PropNav`:

* same sections (`[player]`, `[enemy]`), same property names, same "blank = vanilla"
  rule, same longest-key-first model matching, same legacy `msl = 0.8` shorthand;
* same five tunables: `HomingForesightAmount`, `MaxHomingAngle`, `MaxRotationAngle`,
  `LifeTime`, `LockonRange`;
* plus optional `[runtime]` / `[vanilla]` sections for calibration on an updated build.

## What it does

| Layer | Effect |
| --- | --- |
| **Plain-MSL PN** (on by default) | Detours `FUN_14013ECA0` (the per-frame world weapon manager) and, after the game has run, rewrites each live missile's position and attitude to fly the `λ̇`-based PN command — rate-limited by `MaxRotationAngle`, gated by the seeker cone. |
| **Special-weapon PN** (on by default) | Detours the shared steering integrator `exe+0x90510`; same PN math, applied to QAAM/SAAM/XLAA/XMAA/XAGM. |
| Static parameter patch (optional) | Locates the live missile parameter records by their vanilla-value fingerprint and rewrites range / seeker angle / lock range. |

Vanilla missiles fly pure pursuit — they turn toward the target's **current** position, so
a rate limit plus a seeker cone makes them miss fast crossing targets. Proportional
navigation instead drives the line-of-sight rate (`λ̇`) to zero, i.e. it flies to the
intercept point:

```
λ̇ = (R × Vr) / |R|²          R = target − missile, Vr = targetVel − missileVel
ω = N · λ̇                    N = 4 (tunable via PnGain)
turn per frame = min(|ω|, MaxRotationAngle × dt)
```

**PN owns the whole flight; the terminal phase is not handed back to the vanilla law.**
The close-range `λ̇` singularity is bounded by `rmin = max(2 frames of travel, 25 m)`
(see [docs/MSL_PN.md](docs/MSL_PN.md)).

`HomingForesightAmount` is the **pure-pursuit ↔ true-PN blend**: `0` = pure pursuit
(still rate-limited by `MaxRotationAngle`), `1` = full PN, values in between interpolate
the two resulting headings. The math, the classic `N = 4` law and a 198-engagement sweep
(crossing/beam hit rate **38.9 % → 57.4 %**) are in [docs/PN.md](docs/PN.md).

## Install

1. **Close the game.**
2. **Unzip the release into `ACZero\`** — the folder that contains `acz.exe`:

   ```
   ...\steamapps\common\ACE COMBAT ZERO THE BELKAN WAR (Steam Version)\ACZero\
   |- acz.exe
   |- XINPUT1_4.dll      (loader/proxy, forwards to System32\XInput1_4.dll)
   |- ACZPropNav.dll     (the mod)
   '- ACZPropNav.ini     (configuration)
   ```

   That is the whole install: no installer, no admin rights.

3. Launch from Steam as usual.

**Uninstall**: delete `XINPUT1_4.dll` and `ACZPropNav.dll` (you can keep the INI and
logs). `acz.exe`, every `*.arc` and your save files are never modified.

> `XINPUT1_4.dll` is a transparent proxy of the system XInput DLL; it exists only to get
> our DLL loaded early, and forwards every controller call unchanged to
> `System32\XInput1_4.dll`.

If you already use the community `version.dll` loader (`ACZero\mods\`), you can drop
`ACZPropNav.dll` + `ACZPropNav.ini` into `ACZero\mods\` instead — this DLL also exports
the community addon entry `AczAddonInit`.

### Confirming it works

The default config is intentionally quiet. To see proof, set `LogLevel = 2` in the
`[runtime]` section of `ACZero\ACZPropNav.ini`, then read `ACZero\ACZPropNav.log`:

```
[CORE] ACZPropNav 1.0.0
[CORE] MSLPN: HOOK exe+0x13ECA0 installed (...)
[CORE] MSLPN: full PN for the whole flight: no terminal hand-back (TerminalZone=0)
```

## Troubleshooting / Issues

**Bug reports are welcome, but this is a personal project maintained on a
best-effort basis — updates are sporadic and response times are not guaranteed.**

Before opening an issue (otherwise it usually cannot be diagnosed):

1. In `ACZero\ACZPropNav.ini`, set **`LogLevel = 2`** in `[runtime]` (add the section
   if it is not there).
2. Start the game and **reproduce the problem**.
3. Attach **both** `ACZero\ACZPropNav.log` and `ACZero\ACZPropNav.key.log`.
4. Say which game version, what other mods you use, what you were doing when it broke,
   and which numbers in `ACZPropNav.ini` you changed.

**Never attach game binaries or `*.arc` contents** — that is copyrighted game data.

## Configure

```ini
[player]
msl = {
    HomingForesightAmount = 0.7     ; 0 = vanilla tail chase, 1.0 = full lead
    LockonRange           =
    MaxRotationAngle      =
    MaxHomingAngle        =
    LifeTime              =
}
qaam = { HomingForesightAmount = 1.0 }
2aam = 0.9                          ; legacy shorthand
```

Property → ACZ data field mapping (see [docs/VANILLA.md](docs/VANILLA.md)):

| Property | Unit | ACZ field |
| --- | --- | --- |
| `HomingForesightAmount` | 0 – 1 | `lead` (frames of lead at 60 Hz) |
| `MaxHomingAngle` | deg | `startAng` (seeker start angle) |
| `MaxRotationAngle` | deg/s | *not in the data records* — used only as the rate limit of the PN override |
| `LifeTime` | s | `range` / guidance time-out |
| `LockonRange` | m | `scope` |

INIs written for AC8PropNav keep working unchanged; only the numbers need retuning.

Optional extension sections:

```ini
[runtime]
LogLevel = 2
DryRun   = 0
Probe    = 1        ; deep one-shot dump into ACZPropNav.key.log
LeadScale= 260      ; frames of extra lead added at HomingForesightAmount = 1
LeadMode = add      ; add | blend | absolute
HookRva  = 0x0      ; calibrated address of the missile update
HookSignature =     ; or an AOB pattern
HookArgIndex = 2    ; 1 = this only, 2 = this + float dt

[vanilla]
msl = { lead = 40  scope = 1600  range = 3000  speed = 1000 }
```

## Why runtime discovery

`acz.exe` is wrapped by Steam DRM (the `.bind` stub decrypts `.text` at launch), so there
are no usable static offsets, and the `*.arc` archives hold data, not the guidance loop.
Like AC8PropNav, the mod therefore injects a DLL, reads the INI, locates the structures
in live memory by matching the vanilla values, cross-validates the layout with a second
missile model, and only then writes — every write is read back and verified.
If a game update breaks the fingerprints, run `Probe=1` and follow
[docs/CALIBRATION.md](docs/CALIBRATION.md); nothing is written until the layout verifies.

## Build & test

```bash
./build.sh          # everything into build/
./build.sh test     # build + run all offline tests
```

Offline suites (no game required): `initest` (INI parser vs. the real AC8PropNav.ini,
254 assertions), `pnsim --selftest` (PN math + 198-case engagement sweep),
`memtest` (memory layer + discovery on a synthetic image), `hooktest` (inline hook engine,
accept/refuse rules, proxy forwarding).

## Status, honestly

* ✅ **PN is verified in-game** on both paths: plain MSL (`FUN_14013ECA0` post-update hook)
  and special weapons (`0x90510` integrator) steer and lead as intended.
* ✅ All offline tests pass; DLLs build and load; the configuration is line-by-line
  compatible with AC8PropNav; the PN math has quantitative evidence (198 engagements plus
  self-test assertions).
* 🧪 **Fixed: the blend used to under-turn at mid values.** The old implementation averaged
  the pure-pursuit and PN *rotation vectors*, which are usually not collinear (median
  angle ≈ 45°, >120° in ~5% of geometries), so they partly cancelled: in one geometry
  `amount=0.5` commanded 0.012°/frame while both `0` and `1` commanded 3.0°/frame — and
  the shipped INI uses `0.7`. The blend now interpolates the two resulting *directions*
  (endpoints exact, monotone in `amount`); `pnsim` (k11b1–k11b3) pins it.
* ⚠️ **The terminal phase is no longer handed back to the vanilla law.** A former 30 m
  "quiet zone" existed to dodge a two-missile overshoot bug that turned out to be an
  owner-keyed cache collision (now keyed by slot row). A 972-geometry closed-loop sweep
  shows the hand-back changes no outcome, and `rmin` (33.3 m) already engages earlier, so
  `TerminalZone` defaults to `0` (full PN).
* ⚠️ `MaxRotationAngle` has no analogue in the ACZ data records; it only takes effect while
  PN is in control.
* ⚠️ `LifeTime` / `LockonRange` are not wired on the PN path: the vanilla range/lifetime
  kill and the HUD lock ring still apply.

## Credits

* `AC8PropNav` — the INI contract and the "guide once, maintain cheaply" design this
  project mirrors.
* ACZ Enhanced Gunplay / `aczarc.py` (Volcano Water et al.) — archive tooling, the
  `data/mis/mpl/*.wpt` record schema and the community mod-loader ecosystem.
* [docs/INDEX.md](docs/INDEX.md) — index of every document: which three to read first,
  and which are historical evidence.

No game data or modified game assets are redistributed here.
