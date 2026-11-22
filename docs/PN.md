# ACZPropNav — proportional navigation: math, blend and measured results

This document covers the guidance law in `src/pn/pn.cpp`:

* what vanilla ACE COMBAT missiles do (pure pursuit / tail chase) and why it loses,
* the exact formulas implemented (LOS rate, closing speed, time-to-go, lead point,
  seeker cone, turn-rate limit, the `N · λ̇` rate law),
* how the blend maps onto the AC8-compatible `HomingForesightAmount` knob,
* the offline engagement simulator `tools/pnsim` and the numbers it measures.

> **Which of these runs in the game.** Two paths, two formulations, same law:
>
> * the **special-weapon** hook (`exe+0x90510`) runs exactly what this page describes —
>   `src/pn/pn.cpp::ComputeGuidance`, a lead-point PN
>   (`target + targetVel·tGo·amount`) blended angularly against pure pursuit;
> * the **plain-MSL** hook (`exe+0x13ECA0`) runs the rate form in
>   `src/pn/msl_steer.h::ComputePn` (`ω = N·λ̇`), because it writes a *position* and can
>   therefore command a turn rate directly instead of expressing it as an aim point.
>
> Both drive `λ̇ → 0`, both blend against pure pursuit by `HomingForesightAmount`, and
> both are rate-limited by `MaxRotationAngle`. The *numbers* on this page come from the
> `pn.cpp` formulation, which is the one the simulator measures. For the installed
> behaviour of each path see `docs/MSL_PN.md`.

Everything below is reproducible offline: `./build.sh` builds `build/pnsim.exe`, then
`./build/pnsim.exe --selftest` and `./build/pnsim.exe --report` (on a non-Windows host
`./build.sh test` builds and runs `build/pnsim_native` instead).

---

## 1. Pure pursuit vs proportional navigation

**Pure pursuit** (what the vanilla missile logic does) points the missile at the
target's *current* position, every frame:

```
heading → direction(missilePos → targetPos)
```

It always works in a tail chase and is trivially cheap, but the required turn rate
grows without bound as the missile closes, because the missile keeps chasing a
point that the target keeps moving away from. The missile ends up on a curved
"pursuit curve", arrives with a large heading error, and either misses (outside
the 10 m verification sphere used by the simulator) or runs out of `LifeTime`
first. The classic failure mode is a crossing or beam target: the missile has to
keep turning harder and harder until `MaxRotationAngle` (40 °/s in the
AC8/vanilla-MSIL reference values used by the simulator) runs out and the target
slides out of the seeker cone.

**Proportional navigation** (PN) points the missile at the *intercept point* —
where the target will be when the missile could reach it. Steering to that lead
point makes the line of sight (LOS) stop rotating (λ̇ → 0), which is a collision
course: the missile flies a nearly straight line while the target does all the
work of moving out of the way. The command is expressed as a heading/acceleration
proportional to the LOS rotation rate:

```
γ̇ = N · λ̇            (classic PN rate law)
```

with N ≈ 3…5 (we use `N = 4`, `kPnNavConstant` in `src/pn/pn.h`).

`HomingForesightAmount` interpolates between the two: `0` reproduces the vanilla
tail chase exactly, `1` gives the full collision-course lead.

---

## 2. Geometry and notation

All vectors are world-space, `r` is the missile → target line of sight:

| symbol | meaning |
| --- | --- |
| `r = targetPos − missilePos` | LOS vector (m) |
| `R = |r|` | range (m) |
| `r̂ = r / R` | unit LOS |
| `v_rel = targetVel − missileVel` | relative velocity (m/s) |
| `V_c = dot(missileVel − targetVel, r̂) = −dot(v_rel, r̂)` | closing speed (m/s, > 0 while the range shrinks) |
| `ω = (r × v_rel) / R²` | LOS angular velocity vector (rad/s) |
| `λ̇ = |ω| = |r × v_rel| / R²` | LOS rotation rate (rad/s, non-negative) |
| `t_go = R / V_c` | time to go / time to intercept (s) |
| `γ̇` | missile heading rate (rad/s) |

`dt` is the frame step (`1/120 s` in the game and in the simulator).

---

## 3. Formulas implemented

### 3.1 LOS rotation rate and closing speed — `ComputeLos()`

```cpp
ω      = cross(r, targetVel - missileVel) / |r|^2
lambda = |ω|                       // magnitude only: rad/s
closing = dot(missileVel - targetVel, losUnit)   // > 0 => range shrinking
```

Degenerate input (`|r| < 1e-6 m`, NaN/inf anywhere) returns a zero LOS, zero
rate and zero closing speed instead of dividing by zero.

### 3.2 Pure pursuit — `PursuitDirection()`

```cpp
pursuitDir = normalize(targetPos - missilePos)
```

### 3.3 Lead / intercept point — `ProportionalNavigationDirection()`

```cpp
if (closingSpeed <= 1 m/s || range < 1e-6 m) return pursuitDir;   // no solution
tGo      = clamp(range / closingSpeed, 0, 20)                     // seconds
leadPt   = targetPos + targetVel * (tGo * clamp(lead, 0, 1))
leadDir  = normalize(leadPt - missilePos)
```

The `t_go` clamp to `[0, 20] s` is deliberate: at very low closing speed
(`V_c → 0`, e.g. a target flying away at missile speed) `R / V_c` explodes and the
lead point would be tens of kilometres away. Above 20 s the lead point is
already far enough that the extra length only adds numerical noise. A receding
target (`V_c < 0`) has no intercept solution at all, so the function falls back
to pure pursuit and reports `outTimeToGo = 0`.

### 3.4 The `N = 4` rate law — `PnRateCommand()`

The classic PN heading-rate command, exposed for tuning, documentation and the
self test:

```cpp
cmd = navConstant * lambda        // rad/s, navConstant = kPnNavConstant = 4
```

The *sign* follows the direction the LOS is rotating: the turn axis that swings
the current heading onto the LOS is `a = normalize(heading × r̂)`, and the command
is `+N·λ̇` when `dot(ω, a) > 0`, `−N·λ̇` when `dot(ω, a) < 0`, `0` when the heading
is already on the LOS. In the planar 90° crossing used by the self test that
means: target crossing to the missile's left ⇒ LOS rotates counter-clockwise ⇒
the missile must turn counter-clockwise ⇒ **positive** command.

`pn.cpp`'s own production pipeline (`ComputeGuidance`) does not integrate this rate
directly — it uses the *kinematic* form (aim at the lead point), because that is the form
this module exposes for documentation and for the simulator below. Both forms drive
λ̇ → 0 and therefore reach the same collision course; the `N · λ̇` form is the textbook
feedback law and the reference gain for tuning.

The **shipped** hooks do integrate the rate directly: `src/pn/msl_steer.h::ComputePn`
computes `ω = N·λ̇` and turns the velocity by `min(|ω|, thetaMax)` — the same law, applied
as a rate rather than as an aim point. See `docs/MSL_PN.md` "STEP 2".

### 3.5 The blend — `HomingForesightAmount`

`ComputeGuidance()` builds the two endpoint directions and blends them
**angularly on the great circle** (slerp), with weight `w = clamp(lead, 0, 1)`:

```cpp
pursuitDir = PursuitDirection(in)                       // lead = 0 endpoint
leadDir    = ProportionalNavigationDirection(in, 1, tGo) // lead = 1 endpoint
angle      = AngleBetween(pursuitDir, leadDir)
idealDir   = RotateTowards(pursuitDir, leadDir, w * angle)
```

Properties that matter and are asserted in the self test:

* `w = 0` returns `pursuitDir` **bit-exactly** (no re-normalisation blur), so a
  user with `HomingForesightAmount = 0` gets exactly the vanilla behaviour;
* `w = 1` returns `leadDir` bit-exactly;
* `0 < w < 1` is strictly between the endpoints, and the split is linear in
  *angle*: `angle(pursuit, ideal) = w · angle(pursuit, leadDir)`,
  `angle(ideal, leadDir) = (1 − w) · angle(pursuit, leadDir)`;
* the blend is a rotation of two *unit directions*, so the result is always a
  unit direction — it can never flip or collapse through the target point.

Why angular and not a lerp of lead points? `ProportionalNavigationDirection()`
also exposes the "partial advance" form `leadPt(w) = target + targetVel · tGo · w`
(useful for experiments and identical at the endpoints), but a vector lerp of the
two *aim points* is not a fraction of the *angle* and becomes ill-conditioned
when the target is close. The angular form is the one that behaves like a dial:
half the foresight is half the lead angle. `ProportionalNavigationDirection(..., lead)`
remains the partial-advance helper.

> **Read this before changing the blend.** Blending *rotation-rate vectors*
> (`omegaP*(1−a) + omegaPN*a`) is the obvious implementation and it is wrong: the
> pursuit axis (`uV × uR`) and the PN axis (`R × Vr`) are usually not collinear (over a
> 44,655-geometry grid the median angle is ≈45°, and it exceeds 120° in ~5 % of cases),
> so opposing vectors nearly cancel and a mid `amount` commands *less* turn than either
> endpoint — at bearing 5° / target heading 285° / R=500 m / Vt=750 m/s, `amount=0.5`
> turned 0.012°/frame while both endpoints turned 3.0°/frame. Composing two partial
> rotations in sequence fails too (the net swept angle returns to ~0). The shipped code
> therefore interpolates the two *resulting directions* along the great circle, with each
> endpoint individually clamped to the per-frame budget. `pnsim` (k11b1–k11b3) pins the
> three invariants. See `docs/MSL_PN.md` "STEP 2" for the shipped formula.

### 3.6 Seeker cone (`MaxHomingAngle`) and turn limit (`MaxRotationAngle`)

Per frame, after the ideal direction is known:

```cpp
losAngleDeg = angle(missileHeading, losUnit)      // current LOS, not the lead point
inFov       = (MaxHomingAngle >= 359.9) || (losAngleDeg <= MaxHomingAngle)
turnDeg     = inFov ? min(angle(heading, idealDir), MaxRotationAngle * dt) : 0
desiredDir  = inFov ? RotateTowards(heading, idealDir, turnDeg) : heading
```

* Outside the cone the missile is **ballistic** for that frame: `turnDeg = 0`,
  `desiredDir = current heading`, but `valid` stays `true` — the caller can keep
  the entity alive and log the loss. A full 360° setting is treated as
  "always in FOV", matching the AC8 INI comment *"360 = almost never loses the
  target"*.
* The turn is rate limited **before** it is applied, so a single frame can never
  rotate the missile more than `MaxRotationAngle · dt` (40 °/s / 120 Hz = 0.333°).
* Degenerate samples (zero missile velocity, coincident positions, NaN/inf,
  overflowing range, negative `dt`, non-finite parameters) return
  `valid = false`, a zero `desiredDir` and all-zero finite fields. There is no
  UB, no division by zero and no exception path (built with `-fno-exceptions`).

---

## 4. Mapping to `HomingForesightAmount` (AC8 semantics)

The reference mod `AC8PropNav` (ACE COMBAT 8) exposes
`HomingForesightAmount` per weapon model, `0 = 原版，纯追踪 / vanilla, pure
pursuit`, `1.0 = 提前量拉满 / full lead`. Its shipped INI uses:

| AC8 model | `HomingForesightAmount` |
| --- | --- |
| `qaam` | 1.00 |
| `2aam` | 0.90 |
| `laam`, `4aam` | 0.85 |
| `hvaa`, `hcaa`, `saam`, `sasm`, `6aam` | 0.80 |
| `msl` (player default) | 0.70 |
| `hpaa` | 0.60 |
| `msl` (enemy) | 0.30 |

Vanilla seeker limits quoted in that INI: MSL FOV 75°, QAAM 120°, LAAM 90°,
HVAA 65°, 4AAM 65°; turn MSL 40 °/s, QAAM 75, LAAM 150, HVAA 55, 4AAM 45;
MSL `LifeTime` 9 s ≈ 9.7 km range; MSL `LockonRange` 2000 m.

Our `HomingForesightAmount` is the same 0…1 knob with the same meaning, feeding
the angular blend of §3.5. `src/core/patcher.cpp` resolves it per side
(`[player]` / `[enemy]`) and per model exactly like the AC8 mod.

### ACZ-specific note: the engine's `lead` field is **not** a foresight parameter

The shipping ACZ missile records carry a field called `lead` (observed values: MSL 40,
a fighter-missile variant 80, an older variant 10). It is tempting to read that as the
ACZ equivalent of `HomingForesightAmount` — and the early design in this document did
exactly that. **It is wrong.** A hardware read-watch on the live record showed it is read
per frame by the plain-MSL guidance function, and the disassembly shows what it feeds:

```
maxTurnPerFrame = lead·π/180 / fps          // fps = *(u32*)(ctrl+0x6EC) = 60
```

i.e. **`lead` is a turn-rate clamp in degrees per second**, not a lead distance.
Vanilla MSL 40 °/s lines up exactly with the AC8 `MaxRotationAngle` vanilla value (also
40 °/s); `mis_old` trading reach for `lead` 40→10 is "slower turning", not "less
foresight". Consequences:

* Raising `lead` makes the missile turn harder, but it still aims at the target's
  **current** position — it cannot lead a crossing target.
* So there is no data-field shortcut to PN on this game: the guidance loop has to be
  taken over, which is what `docs/MSL_PN.md` describes.
* A data-type patch can still usefully change `scope` / `range` / `startAng` /
  `attack` (`docs/VANILLA.md`), just not create PN.
* `[runtime] LeadScale` / `LeadMode` exist only for that (now opt-in) static path and
  are **not** handling knobs — see `docs/CALIBRATION.md` §6.

This document's simulator validates the **guidance law and its blending behaviour**:
that "partial lead" is well defined, monotone and bounded. It cannot prove what the game
does internally — that came from the in-game runs.

---

## 5. Production pipeline (pseudocode)

`GuidanceResult ComputeGuidance(in, params)` — the `pn.cpp` reference pipeline, called by
the simulator and available to any future adapter. **The two shipping hooks do not call
this**; they run `msl_steer.h::ComputePn` (`docs/MSL_PN.md` "STEP 2"), which is the rate
form of the same law. One call per missile per frame:

```
if (!allFinite(in) || dt < 0 || !allFinite(params))        -> valid=false, dir=0
heading = normalize(missileVel)                             -> empty: valid=false
R = |targetPos - missilePos|                                (R < 1e-6: valid=false)

pursuitDir = PursuitDirection(in)
leadDir    = ProportionalNavigationDirection(in, lead=1, out tGo)
w          = clamp(HomingForesightAmount, 0, 1)
idealDir   = slerp(pursuitDir, leadDir, w)

losAngleDeg = angle(missileVel, r̂)
inFov       = MaxHomingAngle >= 359.9 || losAngleDeg <= MaxHomingAngle
turnDeg     = inFov ? min(angle(heading, idealDir), MaxRotationAngle * dt) : 0
desiredDir  = inFov ? rotateTowards(heading, idealDir, turnDeg) : heading

mode = w == 0 ? Pursuit : (w == 1 ? ProportionalNavigation : Blended)
```

`mode` is reported so the injector log can show which branch a missile is on;
`timeToGo` is the `t_go` actually used by the lead point (0 when the solution
fell back to pursuit).

---

## 6. The offline simulator (`tools/pnsim`)

`tools/pnsim/main.cpp` is a deterministic planar (x/y) engagement simulator that
calls exactly the same `ComputeGuidance()`:


* missile: constant speed **680 m/s**, starts on the +x boresight at the origin;
* target: constant speed, straight line or constant-rate turn (CCW), no
  evasive manoeuvres;
* guidance: `dt = 1/120 s`; hit when the closest approach within a step is
  **≤ 10 m** (computed exactly from the relative motion over the step, so a fast
  crossing cannot tunnel through the 10 m sphere);
* timeout: `LifeTime` (9 s default) or 60 s hard ceiling;
* no `rand()`, no RNG, no threads — identical inputs give identical outputs
  (asserted by self test `(h)`).

Seeker defaults used by the sweep are the AC8/vanilla-MSIL-like values quoted in
the reference INI: **FOV 75°, turn 40 °/s, lifetime 9 s**. Sweep **B** repeats the
same grid with **FOV 360°** to remove seeker-cone losses and isolate the guidance
law itself.

**Scenario grid** — 9 geometries × 3 ranges × target speeds {200…900} m/s ×
target turn rates {0, 6, 15} °/s = **198 scenarios**, each flown with
`HomingForesightAmount = 0.0 / 0.7 / 1.0`:

| geometry | target bearing / heading (deg) | ranges (m) | target speeds (m/s) |
| --- | --- | --- | --- |
| head-on | 0 / 180 | 1200, 1800, 2600 | 200, 300, 450 |
| tail-chase slow | 0 / 0 | 1200, 1800, 2600 | 250, 350 |
| tail-chase equal | 0 / 0 | 1200, 1800, 2600 | 680 |
| tail-chase fast | 0 / 0 | 1200, 1800, 2600 | 900 |
| crossing 90° | 35 / 125 | 1000, 1600, 2400 | 200, 300, 450 |
| beam | 45 / 90 | 1000, 1600, 2400 | 200, 300, 450 |
| high offset 60° | 60 / 180 | 1000, 1600, 2400 | 200, 300, 450 |
| crossing 90° tight | 40 / 130 | 700, 1000, 1400 | 350, 450, 550 |
| beam hard | 50 / 140 | 800, 1200, 1700 | 400, 500, 600 |

Modes: `--selftest` (assertions, non-zero exit on failure), `--report` (the
tables below; also the default), `-v` (adds one line per simulated case).

---

## 7. Measured results

### 7.1 Self test — `./build/pnsim.exe --selftest`

Two layers: `(a)`–`(h)` cover the `pn.cpp` lead-point core, `(i*)`–`(k*)` cover the
`msl_steer.h` λ̇ form the shipping hooks run. First block, verbatim:

```
=== pnsim self test: pn.cpp guidance core + plain-MSL steer math ===
PASS  (a) lead=0 pipeline == PursuitDirection exactly, every step of a tail chase
PASS  (a2) lead=0 offset tail chase: desiredDir == pursuit whenever not rate limited
PASS  (b) crossing geometry where pure pursuit misses by >100 m but full PN hits
      witness: crossing 90  R0=2400 m  bearing=35 deg  target heading=125 deg  Vt=450 m/s  target turn=15 deg/s
               pursuit: miss, closest 190.9 m, flew 9.01 s  |  PN: hit at 4.19 s
      over crossing/beam scenarios: PN-only kills = 17, pursuit-only kills = 0
PASS  (c) turn per frame <= MaxRotationAngle * dt (commanded and measured)
PASS  (d) target outside MaxHomingAngle => inFov=false, turn=0, heading unchanged
PASS  (d2) MaxHomingAngle >= 360 deg means always in FOV
PASS  (e) degenerate / non-finite input => valid=false and no NaN
PASS  (e2) ComputeLos on coincident positions => zero LOS, zero rates, finite
PASS  (f) lead in (0,1) is angularly strictly between pursuit and full PN
PASS  (g) ComputeLos rate/closing match the closed-form values
PASS  (g2) PnRateCommand = +N * lambda_dot (positive = toward the rotating LOS)
PASS  (g3) closingSpeed > 0 head-on, < 0 when the target outruns the missile
PASS  (g4) receding target => tGo = 0 and lead direction == pursuit direction
PASS  (g5) collinear tail chase => LOS rotation rate ~ 0
PASS  (h) simulator is deterministic (same case twice -> identical outcome)
PASS  (i1) msl_steer::Compute valid for a simple pursuit geometry
...
PASS  (k14c) that turn is non-zero and rate limited
PASS  (k13d) terminal sweep: commanded turn stays below a quarter turn
--- 69 checks, 0 failure(s) ---
```

The `(i*)`–`(k*)` block is what pins the shipped behaviour: `(k11a1)/(k11a2)` the
frame-time/units fix (90 °/s at 60 fps = 1.50 °/frame, was 3× too big),
`(k11b1)–(k11b3)` the direction-blend invariants, `(k13a–d)` the `rmin` terminal guard,
`(k14a–c)` the anti-parallel (target dead astern) case.

### 7.2 Sweep A — vanilla ACZ MSL seeker (FOV 75°, 40 °/s, 9 s)

`meanMiss` = mean closest approach **over the misses only** (the actual miss
distance); `medClosest` = median closest approach over **all** cases (0–10 m ⇒
a kill); `meanT` = mean time to kill; `fov%` = share of cases that lost the
seeker cone at least once.

| family | lead | cases | hit% | meanMiss (m) | medClosest (m) | meanT (s) | fov% |
| --- | --- | --- | --- | --- | --- | --- | --- |
| head-on | 0.00 | 27 | 59.3 | 37.9 | 9.3 | 1.8 | 40.7 |
| | 0.70 | 27 | 96.3 | 12.7 | 5.9 | 1.9 | 3.7 |
| | 1.00 | 27 | 100.0 | -- | 6.7 | 1.9 | 0.0 |
| tail-chase slow | 0.00 | 18 | 83.3 | 17.9 | 8.4 | 4.7 | 16.7 |
| | 0.70 | 18 | 94.4 | 11.9 | 9.2 | 4.7 | 5.6 |
| | 1.00 | 18 | 100.0 | -- | 9.2 | 4.7 | 0.0 |
| tail-chase equal | 0.00 | 9 | 0.0 | 1575.1 | 1455.0 | 0.0 | 0.0 |
| | 0.70 | 9 | 0.0 | 1349.8 | 1200.0 | 0.0 | 0.0 |
| | 1.00 | 9 | 0.0 | 1400.8 | 1220.0 | 0.0 | 33.3 |
| tail-chase fast | 0.00/0.70/1.00 | 9 | 0.0 | 1866.7 | 1800.0 | 0.0 | 0.0 |
| crossing 90 | 0.00 | 27 | 33.3 | 236.2 | 42.4 | 5.9 | 66.7 |
| | 0.70 | 27 | 55.6 | 318.8 | 9.9 | 4.8 | 44.4 |
| | 1.00 | 27 | 63.0 | 378.5 | 9.6 | 4.7 | 37.0 |
| beam | 0.00 | 27 | 55.6 | 132.3 | 9.6 | 5.6 | 37.0 |
| | 0.70 | 27 | 74.1 | 197.6 | 9.4 | 5.2 | 22.2 |
| | 1.00 | 27 | 77.8 | 227.0 | 8.6 | 5.2 | 18.5 |
| high offset 60 | 0.00 | 27 | 14.8 | 569.2 | 622.6 | 7.1 | 77.8 |
| | 0.70 | 27 | 29.6 | 679.8 | 622.6 | 5.8 | 63.0 |
| | 1.00 | 27 | 29.6 | 680.2 | 622.6 | 5.8 | 63.0 |
| crossing 90 tight | 0.00 | 27 | 22.2 | 484.9 | 520.8 | 6.9 | 59.3 |
| | 0.70 | 27 | 29.6 | 524.7 | 513.7 | 6.6 | 51.9 |
| | 1.00 | 27 | 33.3 | 558.8 | 520.8 | 6.4 | 48.1 |
| beam hard | 0.00 | 27 | 18.5 | 644.5 | 677.7 | 6.8 | 37.0 |
| | 0.70 | 27 | 18.5 | 630.0 | 677.7 | 6.8 | 37.0 |
| | 1.00 | 27 | 18.5 | 641.8 | 677.7 | 6.8 | 37.0 |
| **ALL FAMILIES** | 0.00 | 198 | **35.4** | 583.9 | 48.2 | 4.9 | 44.9 |
| | 0.70 | 198 | **50.0** | 716.4 | 10.3 | 4.4 | 30.8 |
| | 1.00 | 198 | **53.0** | 770.5 | 9.9 | 4.4 | 29.3 |
| of which crossing/beam | 0.00 | 108 | **32.4** | 413.7 | 97.2 | 6.1 | 50.0 |
| | 0.70 | 108 | **44.4** | 484.0 | 45.9 | 5.5 | 38.9 |
| | 1.00 | 108 | **48.1** | 523.7 | 45.9 | 5.4 | 35.2 |

### 7.3 Sweep B — FOV 360°, isolating the guidance law

Same 198 scenarios, seeker cone removed (no `fov%` column losses). This is the
fair comparison of the laws themselves:

| family | lead | cases | hit% | meanMiss (m) | medClosest (m) | meanT (s) |
| --- | --- | --- | --- | --- | --- | --- |
| head-on | 0.00 / 0.70 / 1.00 | 27 | 59.3 / 96.3 / 100.0 | 37.9 / 12.7 / -- | 9.3 / 5.9 / 6.7 | 1.8 / 1.9 / 1.9 |
| tail-chase slow | 0.00 / 0.70 / 1.00 | 18 | 83.3 / 94.4 / 100.0 | 17.9 / 11.9 / -- | 8.4 / 9.2 / 9.2 | 4.7 / 4.7 / 4.7 |
| tail-chase equal | 0.00 / 0.70 / 1.00 | 9 | 0.0 / 0.0 / 11.1 | 1575.1 / 1349.8 / 1536.5 | 1455.0 / 1200.0 / 1200.0 | -- / -- / 8.9 |
| tail-chase fast | 0.00 / 0.70 / 1.00 | 9 | 0.0 / 0.0 / 0.0 | 1866.7 | 1800.0 | -- |
| crossing 90 | 0.00 / 0.70 / 1.00 | 27 | 33.3 / 59.3 / 66.7 | 140.2 / 186.6 / 228.2 | 42.0 / 9.8 / 9.2 | 5.9 / 5.0 / 4.8 |
| beam | 0.00 / 0.70 / 1.00 | 27 | 55.6 / 74.1 / 77.8 | 128.1 / 190.6 / 220.5 | 9.6 / 9.4 / 8.6 | 5.6 / 5.2 / 5.2 |
| high offset 60 | 0.00 / 0.70 / 1.00 | 27 | 22.2 / 40.7 / 40.7 | 370.2 / 471.7 / 478.7 | 200.3 / 200.3 / 200.7 | 7.5 / 6.4 / 6.4 |
| crossing 90 tight | 0.00 / 0.70 / 1.00 | 27 | 40.7 / 51.9 / 59.3 | 358.9 / 405.5 / 493.1 | 61.5 / 9.8 / 9.2 | 7.4 / 7.0 / 6.8 |
| beam hard | 0.00 / 0.70 / 1.00 | 27 | 25.9 / 25.9 / 25.9 | 606.0 / 586.7 / 601.9 | 393.8 / 394.8 / 402.7 | 7.1 / 7.0 / 7.0 |
| **ALL FAMILIES** | 0.00 / 0.70 / 1.00 | 198 | **39.9 / 56.1 / 60.1** | 513.8 / 654.2 / 729.0 | 32.6 / 9.8 / 9.3 | 5.2 / 4.8 / 4.8 |
| of which crossing/beam | 0.00 / 0.70 / 1.00 | 108 | **38.9 / 52.8 / 57.4** | 332.2 / 399.8 / 453.0 | 42.0 / 9.9 / 9.4 | 6.4 / 5.8 / 5.7 |

Reading notes (also printed by `--report`):

* `meanMiss` is **conditioned on the misses**. It can look worse for PN because
  PN converts the near misses into kills and leaves only the hopeless geometries
  in its miss set. `hit%` and `medClosest` are the unbiased comparisons.
* The tail-chase families are controls: a straight-line target at 680 m/s or
  900 m/s can never be caught by a 680 m/s missile (0 % for every setting). The
  single exception is an equal-speed target turning at 15 °/s at 1200 m: PN cuts
  inside its turn and reaches 4.5 m at 8.87 s (the one 11.1 % row in sweep B).
  With the real 75° cone the seeker is lost there and every equal-speed case
  misses — so this is a "hard but possible" row, not a regression.
* The `beam hard` family (Vt 400–600 m/s at 800–1700 m) is a control too: both
  laws fail, the geometry is outside the missile's kinematics with a 9 s life.

---

## 8. Conclusion

* With the **vanilla 75° cone, 40 °/s, 9 s** seeker, pure pursuit kills
  **35.4 %** of the 198 scenarios and full PN **53.0 %**; on the discriminating
  crossing/beam subset **32.4 % → 48.1 %**.
* With the cone removed (FOV 360°, so only the law is judged) the same sweep goes
  **39.9 % → 60.1 %** overall and **38.9 % → 57.4 % (+18.5 pp)** on crossing/beam.
* The gain is largest where pure pursuit structurally fails: crossing 90°
  **33.3 % → 66.7 %**, beam **55.6 % → 77.8 %**, crossing 90° tight
  **40.7 % → 59.3 %**, head-on **59.3 % → 100 %** (sweep B).
* PN also gets there faster and more cleanly (sweep A, crossing 90°): median
  closest approach **42.4 m → 9.6 m**, mean time to kill **5.9 s → 4.7 s**, and
  seeker cone losses drop from **66.7 % → 37.0 %** of cases.
* The blend behaves as a dial: `lead = 0.7` is usually within a few points of
  full PN (e.g. 52.8 % vs 57.4 % on crossing/beam) while staying noticeably
  softer than `lead = 1`, which matches the AC8 mod shipping `0.7` for the
  player MSL and `1.0` only for QAAM.
* Witness geometry: crossing 90°, R0 = 2400 m, target 450 m/s, 15 °/s turn —
  pure pursuit misses by **190.9 m** after the full 9 s life; PN hits at
  **4.19 s**. This is the exact case asserted by self test `(b)`.
* No regressions in the invariants: `lead = 0` is bit-identical to
  `PursuitDirection()` every frame, the per-frame turn is always
  ≤ `MaxRotationAngle · dt`, out-of-cone targets are never rotated, and every
  degenerate/non-finite input yields `valid = false` with finite fields.

---

## 9. What is measured, what is assumed, what is *not* proven

**Measured (reproducible offline, this document's tables):** the behaviour of
`src/pn/pn.cpp` on the input the injector feeds it — LOS rate, closing speed,
`t_go`, lead direction, angular blend, seeker gate, turn-rate limit, and closed-loop
engagement outcomes (hit rate, miss distance, time to kill, cone losses) for a
constant-speed missile against straight/turning targets.

**Modelling choices of the simulator (not game measurements):**

| item | value used | why |
| --- | --- | --- |
| missile speed | 680 m/s constant | ACZ-plausible; no drag/thrust/energy model, no gravity or motor burn |
| world | planar 2-D (x/y) | the guidance law is rotation-covariant; a 2-D sim exercises every code path without pretending to model altitude |
| target motion | straight line or constant turn rate 0/6/15 °/s | no evasive maneuvers, no F-pole, no chaff/flares |
| seeker | FOV 75°, 40 °/s, 9 s | AC8 vanilla MSL values quoted by `AC8PropNav.ini`, kept as the sweep's stand-in (the sweep isolates the law; the shipping modulator is `HomingForesightAmount` + `MaxRotationAngle`, not these) |
| hit criterion | closest approach ≤ 10 m, interpolated within the step | no proximity-fuse delay, no warhead lethality model |
| guidance rate | one `ComputeGuidance()` per 1/120 s frame | the simulator's fixed step (the game runs at 60 fps and the hooks use `1/gameFps`) |
| FOV loss | ballistic for that frame, no re-acquisition | mirrors the AC8 INI description ("outside the cone is NOT tracked") |
| `t_go` clamp | [0, 20] s | avoids an unbounded lead point at near-zero closing speed |
| `N` | 4 (`kPnNavConstant`) | textbook constant-speed sweet spot; only used by `PnRateCommand()` and the self test, not by the aim-point pipeline |

**Established in-game since (so no longer open):**

* The plain MSL's per-frame guidance function, its slot layout, and that `lead`
  (`P+0x2C`) is a **turn-rate clamp** rather than a foresight value — see §4's ACZ note
  and `docs/MSL_PN.md`.
* That taking over the guidance loop works: both paths (plain MSL and special weapons)
  were verified in live sorties, with the PN correction applied every frame.

**Still not proven here:**

* ACZ's exact missile speed, `LifeTime` and `LockonRange` values as *guidance* inputs —
  the PN path does not use them (the vanilla range/lifetime kill still runs), so the
  simulator's stand-ins do not affect the shipped behaviour.
* Real-world PK against manoeuvring players/AI; the simulator's targets never
  react, and the 10 m sphere is a stand-in for the game's own hit test.

The self test and the sweep exist to make the *guidance law* falsifiable: every
claim above is one command away, with deterministic output.

---

## 10. Files

| file | role |
| --- | --- |
| `src/pn/pn.h` | interface, `kPnNavConstant = 4` |
| `src/pn/pn.cpp` | all formulas in this document |
| `src/pn/msl_steer.h` | the λ̇ form the shipped hooks actually run (`ComputePn`) |
| `tools/pnsim/main.cpp` | deterministic engagement simulator + self test (`--selftest`, `--report`, `-v`) |
| `docs/ARCHITECTURE.md` | module contracts, where `ComputeGuidance` is called |
| `docs/VANILLA.md` (arc-recon) | ACZ vanilla missile records, to replace the stand-in seeker values |
