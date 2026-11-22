# MSL_PN — plain-MSL proportional navigation (post-update hook on `exe+0x13ECA0`)

Status: **implemented, in-game verified, offline-green** (2026-11-21). `./build.sh test`:
initest 268 / **pnsim 62** / memtest 85 / hooktest 98 / hwwatchtest 27 / smoke PASS.

Files: `src/pn/msl_pn.h`, `src/pn/msl_pn.cpp` (hook + slot scan + logging),
`src/pn/msl_steer.h` (pure PN math, unit-tested by `pnsim`), wiring in
`src/core/patcher.cpp` (`Start` / `Tick` / `Shutdown`), prologue test
`hooktest` Test8, math tests `pnsim` (i1)–(i16).

## Why this target

Run B's hardware read-watch (`docs/RUN_B_RESULTS.md`) showed that
`FUN_14013ECA0` (**RVA `0x13ECA0`**) reads the live `.wpb` record
`P = *(node+0x28)` every frame: `P+0x2C` (lead) 88 times at
`exe+0x13EFA2` (`vmovss xmm0,[rax+0x2c]`, `rax = P`), plus `P+0x4A`
(startAng) and `P+0x14` (range). Static verification (re-msl) then pinned the
function's data model:

| fact | value |
| --- | --- |
| `ctrl` (rcx) | **world-level weapon manager** = `world+0x6A130` (one call per frame, not per aircraft) |
| call site | `0x1415DD` in `FUN_140141590`, gated by `[world+0xA8790] & 0x10000000` |
| live-slot mask | `u32` at `ctrl+0x6E8` |
| slot | `ctrl+0x158 + i*0x58`, `i = 0..15` |
| `slot+0x00` | position vec4 (`+0x0C` w = 1.0f) — **the integrator adds the displacement here** |
| `slot+0x10` | quaternion (orientation; not written by this mod) |
| `slot+0x20` | per-frame displacement vec4 — **output cache, recomputed every frame** |
| `slot+0x30` | `qword` → `P` (the `.wpb` record; `P-0x10 u32 data_off == 0x10`, `P+0x48 u8 type == 2`) |
| `slot+0x38` / `+0x3C` | traveled distance / turn accumulator |
| `slot+0x40` | `u16` trail handle |
| `slot+0x42/+0x43` | target (obj, part) indices, `0xFF` = none |
| `slot+0x44/+0x45` | owner (obj, part) indices — per-aircraft identity |
| `ctrl+0x6EC` | constant `60` (game FPS) |
| lead units | `P+0x2C` is in **degrees**; vanilla per-frame clamp ≈ `lead*π/180/60` |

Because the game adds `slot+0x20` to `slot+0x00` itself, steering **must write the
position**, not `+0x20`. The mod therefore runs post-update: the vanilla code
integrates the frame normally, then we move the missile to where the PN
displacement (same speed, rate-limited turn) would have put it.

## Mechanism

* **Hook**: `exe+0x13ECA0` entry, 15 stolen bytes
  `48 8B C4 | 48 89 58 08 | 48 89 70 18 | 48 89 78 20` (mov rax,rsp + three
  home-space spills; instruction boundary at `0x13ECAF`, no RIP-relative operand).
  Byte-verified before every install attempt; `hooktest` Test8 pins it offline.
* **Deferred install** in `Patcher::Tick` (200 ms retry until SteamStub has
  decrypted `.text`, 60 s cap; the game is untouched if it never matches).
* **Detour** (`void Detour(void* ctrl)`): trampoline first, then
  `msl_pn::Apply(ctrl)`. The only caller ignores the return value, so the ABI is
  safe; the detour never logs or allocates (it publishes into fixed arrays and a
  16-entry detail ring that `Tick` drains).
* **Apply** (once per frame): for each live slot (`ctrl+0x6E8` bit set):
  1. `owner = slot+0x44` (`0xFF` → skip); player iff `owner == *(u8)(base+0x3409F2)`.
  2. `FS = [player]` / `[enemy]` `msl` block; `amount = HomingForesightAmount`
     (`> 0.001` required).
  3. `tObj/tPart = slot+0x42/+0x43` (`0xFF` → skip); resolve
     `objArray[tObj] -> +0x110` parts `+ 0x520*tPart + 0x258` = target position
     vec4. `objArray` comes from the game's own ctrl-relative path
     `*(u64*)(*(u64*)(ctrl+0x08))` (site `0x13ED70`), with
     `world = *(u64)(base+0x3409F8)`, `objArray = *(u64)(world+0x697B8)` as the
     fallback; object count from `world+0xA87E4`.
  4. Target velocity = position difference between two adjacent game frames
     (per-frame cache keyed `(tObj<<8)|tPart`, 64 entries).
  5. `.wpb` sanity: `P = *(u64)(slot+0x30)`, `*(u32)(P-0x10) == 0x10`,
     `*(u8)(P+0x48) == 2`; `speed_mps = *(u16)(P+0x44)` (fallback 1000).
  6. Missile displacement `v`: `vGame` is read straight from `slot+0x20..0x28` (the step
     the game just applied this frame), and the *guidance basis* is our own cached unit
     heading `dir` from the last frame, scaled to `|vGame|`. The per-slot cache is keyed
     by the **slot row index** (0..15) — see the two-missile fix at the bottom.
     (The older "derive velocity from a per-slot position cache" scheme is gone.)
  7. PN (`src/pn/msl_steer.h`):
     `R = T - p`; `uR = R/|R|`; `vc = |v| - dot(uR, Vt)`, floored at `0.2|v|`;
     `tGo = clamp(|R|/vc, 0, 600)`; `aim = T + Vt*tGo*amount`;
     `d = normalize(aim-p)`; `f = v/|v|`; `theta = acos(dot(f,d))`;
     `dt_game = clamp(|v|/speed_mps, 1/240, 1/20)`;
     `thetaMax = (MaxRotationAngle or 180°) * dt_game`; rotate `f` toward `d` by
     `min(theta, thetaMax)`; `vNew = |v| * dir`.
  8. Write (unless `DryRun=1`): `slot+0x00/+0x04/+0x08 = p + (vNew - v)`
     (**w lane `+0x0C` stays 1.0f**); optionally refresh the output cache
     `slot+0x20..0x28 = vNew`, `slot+0x2C = 0` for intra-frame consistency;
     update the slot cache with the written position/displacement.
* **Never touched** (safety list): `ctrl+0x6E8`, `slot+0x30`, `slot+0x40`,
  `slot+0x42..0x45`.
* **Guards**: hook only when `GuidanceOverride = 1`, at least one side's `msl`
  has `HomingForesightAmount > 0`, and neither `Probe=1` nor `HwWatch=1` is
  active. `DryRun=1` computes and logs but writes nothing.

## INI mapping

| INI key | effect |
| --- | --- |
| `[player] msl HomingForesightAmount` | PN blend for the player's plain MSL: `0` = pure pursuit (rate-limited by `MaxRotationAngle`), `1` = full PN (`N = PnGain`). **Blank = that model is left vanilla**; an explicit `0` still arms the mod in pursuit mode, which is what makes an A/B baseline possible. Ships at `0.7`. |
| `[enemy] msl HomingForesightAmount` | same PN blend for every non-player missile (hostile **and** wingman, since they share the record family). Ships at `0.3` (i.e. mostly pursuit for NPCs). |
| `[player]/[enemy] msl MaxRotationAngle` | per-frame turn limit; blank ⇒ 180 °/s. |
| `[runtime] GuidanceOverride` | master switch (0 = no PN at all). |
| `[runtime] DryRun` | log-only mode. |
| `MaxHomingAngle` | seeker cone HALF-angle in degrees. Overrides the game's own `P+0x4A` (startAng) when set; the PN write is refused (and counted as `outOfCone=`) when the target falls outside it. |
| `LifeTime`, `LockonRange` | **not wired on this path** — the vanilla range/lifetime kill and the HUD lock ring still apply unchanged. |

## Logging (`ACZPropNav.key.log`, all `MSLPN:` prefixed)

> Levels: the startup/armed/HOOK lines and the "called too fast" warning are `Info()`
> and appear at the default `LogLevel = 1` (blank/absent = quiet: startup, hook status,
> warnings/errors). The 2 s summary, the per-correction detail lines and the "not entered
> for >5 s" notice are `Key()` and need `[runtime] LogLevel = 2`. `LogLevel = 0` writes
> no log content at all.

* one init line with the armed hook RVA and the `[player]`/`[enemy]` amounts;
* every 2 s: `calls / slots / player / enemy / computed / applied / badTarget /
  badGeom / writeFail / frame / maxCorr / dryRun`;
* up to 16 `MSLPN: steer tObj=.. tPart=.. amount=.. |R|=.. tGo=.. theta=..
  limit=.. |v|=.. vNew=(..)` detail lines (the first corrections);
* a one-shot warning if `FUN_14013ECA0` is not entered for >5 s (world gate
  `world+0xA8790 & 0x10000000`, pause, loading screen) and a line when it resumes;
* a one-shot warning if the guidance is called faster than ~330 times/s (the per-frame
  heuristic in `Apply` cannot then tell frames apart, so the guidance basis and the
  target-velocity finite difference degrade - normally never fires at 60 fps);
* `outOfCone=` (seeker cone refused the write), `terminal=` (frames inside
  `TerminalZone`, 0 unless opted in) and `staleDir=` (a cached heading disagreed with
  the game by >30 deg, i.e. a slot row was reused) in the 2 s summary;
* `MSLPN: HOOK exe+0x13ECA0 installed …` once the deferred install lands (this one is
  `Info()`, so it shows up even at `LogLevel = 1`).

## Verification recipe (one in-game run)

1. Edit `ACZero\ACZPropNav.ini` and set, in `[runtime]`:
   `GuidanceOverride = 1`, `DryRun = 0`, `Probe = 0`, `RingSurvey = 0`.
   Add `LogLevel = 2` so the 2 s summaries and the correction detail lines are
   actually written (the default `LogLevel = 1` is quiet on purpose).
   (This used to be one click on `enable_pn.cmd`; that script has since been removed.)
2. Fire plain MSL at a crossing (fast lateral) target: the flight path should lead
   the target instead of tail-chasing. Compare with `HomingForesightAmount = 0`
   (vanilla) in the same geometry.
3. `ACZPropNav.key.log` must show `MSLPN: HOOK …`, periodic summaries with
   `computed`/`applied` increasing while a missile flies, and detail lines with a
   non-zero `theta`/`vNew` = the applied correction.
4. If `applied` stays 0: check `badTarget`/`badGeom`/`writeFail` in the summaries
   (the summary counts separate "no target", "geometry rejected" and "write
   failed").

## STEP 1B — slot orientation takeover (Euler, not quaternion)

The earlier "slot+0x10 is a quaternion" assumption was wrong. Step 0 established:

* `FUN_140202FA0(out_mat16, euler4)` — **Euler angles → 4x4 matrix** (uses the game's
  `sincos`); RVA **`0x202FA0`**.
* `FUN_1402036D0(out_euler4, in_euler4, rotvec3)` — **applies a rotation vector
  (axis·angle) to Euler angles**; a zero rotvec just copies; it writes `out[0..2]`
  (the 4th float is copied through). RVA **`0x2036D0`**.
* RVA `0x13ED70`-family checks also pinned `P+0x4A` (startAng) as a **half-angle**
  (`dot(forward, uLOS) > cos(startAng)`, startAng 90° ⇒ 180° cone) and
  `slot+0x3C` as the missile's speed in m/frame (`slot+0x38` is traveled distance).

What the shipping code does now (`src/pn/msl_pn.cpp`):

1. **Input basis (cross-frame caches removed).** After the trampoline, `vGame` is
   read directly from `slot+0x20..+0x28` (the exact step the game applied; its
   direction is the post-integration heading). The only per-slot state is a unit
   vector `dir` = the heading WE commanded last frame; the guidance input is
   `dir * |vGame|`, so PN rotation accumulates instead of resetting to the game's own
   heading every frame. There is no position cache and no direction derivation from
   position differences any more (the old `se->pos`/`se->vel` logic is gone).
2. **Position** (unchanged semantics): `slot+0x00..0x08 = p + (vNew − vGame)`,
   `pos.w (+0x0C)` stays `1.0f`; `slot+0x20..0x28 = vNew`, `slot+0x2C = 0` only as
   intra-frame output-cache consistency.
3. **Orientation sync (new).** With `heading = normalize(vGame)` and
   `d = normalize(vNew)`:
   `axis = cross(heading,d)`, `angle = atan2(|axis|, dot(heading,d))` clamped to
   `[0, π/2]`, `rotvec = normalize(axis)*angle` (zero when `angle < 1e-4`). The slot's
   Euler quad is read, `FUN_1402036D0(out, in, rotvec)` is called, and `out[0..2]` is
   written back to `slot+0x10/+0x14/+0x18` — `+0x1C` and the whole `+0x1A0..` are
   untouched. `slot+0x46/+0x47` are never written (bit 0 of `+0x47` is the game's
   "target valid + guidance applied" flag).
4. **Convention auto-calibration (once per run, scratch memory only, never a live
   missile).** Read the current Euler angles, build the matrix with `FUN_140202FA0`,
   then pick the matrix direction (6 candidates: the 3 row-major columns and the 3
   rows, so either storage order is covered) with the largest dot product against the
   heading. Then try the 4 rotation-vector variants — (0) world axis as computed,
   (1) negated, (2) world→local via the matrix transpose, (3) negated local — apply
   each with `FUN_1402036D0` to a scratch Euler copy, rebuild the matrix and score
   `dot(forward_variant, d)`. The best variant wins if it beats `dot(heading,d)` by
   `1e-4`; the result is logged once as

   ```
   MSLPN: STEP1B orientation calibration: axis=<0..5> variant=<0..3> ok=<0|1>
          base=<dot> scores=[s0 s1 s2 s3]
   ```

   `axis 0..2` = row-major columns, `3..5` = rows. If no direction matches
   (`axisScore < 0.5`) or no variant wins, `ok=0` is logged, **orientation writes are
   disabled for the run**, and position steering continues unchanged. The helper
   entries are byte-checked at runtime against the reference image before the first
   call (`hooktest` Test9 verifies the same bytes offline); a mismatch disables only
   the orientation path.
5. **Counters.** The 2 s summary now also reports `orient=<successful Euler writes>
   orientFail=<failed writes> calib=<variant>/<axis>`.

Both game helpers are pure math and are called on the game thread from the
post-update hook (after the vanilla body), so they cannot interfere with the game's
own state. Their RVAs and expected entry bytes are shared through `src/pn/msl_pn.h`
and verified offline by `hooktest` Test9 (`FUN_140202FA0` 21 bytes,
`FUN_1402036D0` 18 bytes).

## STEP 2 — true lambda-dot PN (replaces the lead-pursuit math)

`HomingForesightAmount` is now a **blend between pure pursuit and true
proportional navigation**, not a lead fraction.  The math lives in
`src/pn/msl_steer.h::ComputePn` (pure, unit-tested by `pnsim` (k1)–(k12)):

```
R = target - p ; r = |R| ; speed = |v| ; uV = v/speed ; Vr = targetVel - v
rmin = max(2 * speed, 25) ; r2 = max(r*r, rmin*rmin)      // soft singularity guard
lambdaDot = Cross(R, Vr) / r2                            // LOS rate (rad/frame)
omegaPN   = N * lambdaDot                                // N = PnGain (default 4)
uR  = R/r ; thetaP = angle(uV -> uR)                     // one-frame pursuit demand
a   = clamp(HomingForesightAmount, 0, 1)
dtGame   = 1 / gameFps        // fps = *(u32*)(ctrl+0x6EC), 60 in this build
thetaMax = MaxRotationAngle * dtGame

// endpoint A: pure pursuit, clamped to the per-frame budget
dirPursuit = rotate(uV, normalize(uV x uR), min(thetaP, thetaMax))

// endpoint B: proportional navigation, clamped to the same budget
dirPN      = rotate(uV, normalize(N * lambdaDot), min(|N*lambdaDot|, thetaMax))

// blend along the great circle between the two resulting directions
span = angle(dirPursuit, dirPN)
dir  = RotateTowards(dirPursuit, dirPN, a * span)
vNew = normalize(dir) * speed
```

> **Do not blend the rotation-rate vectors, and do not merely compose the two
> rotations.** Both were tried here and both are wrong.
>
> 1. `omegaCmd = omegaP*(1-a) + omegaPN*a` averages two *vectors* that are usually
>    not collinear: over a 44,655-geometry grid the angle between the pursuit axis
>    (`uV × uR`) and the PN axis (`R × Vr`) has p50 ≈ 45° and exceeds 120° in ~5% of
>    cases. Opposing vectors nearly cancel, so a mid `amount` commanded *less* turn
>    than either endpoint — at bearing 5° / target heading 285° / R=500 m /
>    Vt=750 m/s, `amount=0.5` turned **0.012°/frame** while both `amount=0` and
>    `amount=1` turned 3.0°/frame. The shipped INI uses `amount=0.7`, i.e. real
>    gameplay sat on the broken part of that curve.
> 2. Composing the two partial rotations in sequence fixes the instantaneous sum but
>    not the disagreement underneath: when the laws pull opposite ways the missile
>    rotates out and straight back, so the **net** swept angle is still ~0 (measured:
>    `amount=0.5` still gave 0).
>
> Interpolating the two *resulting directions* is well defined for any pair: a
> great-circle interpolation always lands between them, endpoints stay exact, and the
> result is monotone in `amount`. Each endpoint is individually clamped to `thetaMax`,
> so their mutual span is ≤ 2·`thetaMax` and every point on it is a legal frame.
> `pnsim` (k11b1–k11b3) pins all three invariants.
>
> Note this means that in a geometry where the two laws genuinely oppose, a middle
> `amount` can legitimately command a *smaller net turn* than either endpoint — the
> heading is a real compromise between them, not an artefact.

> **Frame time comes from the engine, never from the displacement.** The slot positions
> are in the game's own position units, NOT metres: 1 m == 5.12 units (proven by the
> range kill at `0x1402D46B0` = 5.1199999, and a live run logs `|v| ≈ 60.4` units/frame
> ≈ 11.8 m/frame ≈ 707 m/s at 60 fps). An earlier version derived `dt = |v| / speedMps`,
> mixing those units: it produced 0.060 s, saturated the old `[1/240, 1/20]` clamp at
> 0.05 s, and inflated the per-frame turn budget **3x** — a 90 °/s setting behaved like
> 270 °/s (the log printed `limit=4.50deg` where 1.50 is correct). Using `1/fps` is both
> correct and exactly the vanilla convention (`maxTurn = lead_deg·π/180 / fps`).
> Pinned by `pnsim` (k11a1)/(k11a2).

* `amount = 0` ⇒ pure pursuit (the command points at the target's current
  position — the vanilla-like chase).
* `amount = 1` ⇒ full PN with `N = 4` (the classic navigation constant; the
  collision course produces `lambdaDot = 0` and therefore no turn).
* `amount` in between blends the two rotation-rate demands, so an existing INI
  value (ships `[player] msl = 0.7`) becomes "70 % PN, 30 % pursuit".

**Why the Step-1 terminal hardcodes are gone.** The old code tapered a
lead-pursuit aim point over 250 m…1200 m (`taperRange`), tapered by `tGo`
(`taperTgo`) and dropped the lead entirely when the target was behind the missile
within 1500 m.  All three were heuristics compensating for "aim at a lead point"
geometry; true PN is scale-free, so they are deleted.  The only remaining
close-range handling is the `rmin` singularity guard: it clamps `r²` to
`max(2 frames of travel, 25 m)²`, which keeps `lambdaDot` finite and the commanded
turn bounded as `r → 0`.  The amount EMA smoothing (0.7/0.3) and the
target-velocity clamp + 0.6/0.4 low-pass are unchanged.

**Orientation calibration fix.** The Step-1B calibration ran once, on the first
correction; if that correction was only ~0.6° all four rotvec-variant scores were
indistinguishable and orientation was disabled for the whole run.  It now:

* only attempts when `angle(heading, d) > 0.02 rad` (≈1.15°) and the step is sane,
* retries every frame for up to 60 attempts or 6 s (counting the skipped
  tiny-angle frames separately),
* logs the six matrix-axis dots and the four variant scores with full precision
  (`%.9g`), plus the raw heading/desired vectors, and only then decides `ok=0`,
* keeps orientation disabled (position steering unaffected) if it still cannot
  resolve the convention.

New summary fields: `pn=<computed corrections> lambda=<mean>/<max> mrad/f`
(mean/max |lambdaDot| per 2 s window) next to the existing `orient=`/`orientFail=`
counters.  The per-correction detail line now prints `amount, N=4, |R|,
|lambda|, turn, limit, pursuit/pn component magnitudes, |v|, vNew`.

## Limitations / residual risks

* **Orientation takeover is calibrated, not assumed.** Step 1B rotates the slot's
  Euler angles with the game's own helper, but the matrix forward-axis and the
  rotation-vector frame/sign are discovered at runtime; if the calibration is
  ambiguous (`ok=0` in the log) the orientation writes are skipped for that run and
  the model may look slightly misaligned with the (still correctly steered)
  trajectory.
* **The game's own turn clamp is bypassed** by design (we inject positions), so
  the effective turn authority is `MaxRotationAngle * dt_game` from this code and
  the config, not the vanilla `lead` clamp. Balance differs from vanilla even at
  the same `HomingForesightAmount`.
* **Target position may lag one frame** (the dispatcher reads it inside the same
  iteration); the PN aim point uses a per-frame finite difference of that
  position, so a 1-frame lag adds a small bias, not instability.
* **Frame detection** is wall-clock based (a >3 ms gap between `Apply` calls
  starts a new frame id); the function is called once per frame in this build, so
  this is exact at 60 Hz. A non-60 Hz build would still work but the per-frame
  deltas would be scaled differently.
* **Writing costs**: each corrected slot costs one `VirtualProtectEx` pair via
  `mem::WriteVec3` (plus the optional output-cache refreshes). With ≤16 slots and
  only live missiles this is a few thousand syscalls/s worst case; monitoring via
  the `writeFail` counter.
* **Hook install can fail** (different game build / engine refusal): the mod then
  logs it and the plain MSL keeps vanilla guidance for that run; nothing crashes.
* The mod **does not touch** the fire/launch path, so launch lead/aim remains
  vanilla; PN only acts while the missile is in flight.
* `[enemy] msl = 0.3` by default means NPC/wingman MSLs are steered too; set it
  blank/0 to restrict the mod to the player.

## Two-missile fix (2026-10-27) — slot cache was keyed on the owner

*Symptom*: when two MSLs are fired in quick succession, one of them overshoots and
circles rapidly while the other tracks normally.
*Root cause (decompiled)*: the launcher (`FUN_14013d180`) writes the owner pair
`slot+0x44/0x45` from `part+0x234/0x235` of the fire record's part — i.e. every
missile of one aircraft gets the same `(playerIdx, rootPart)`. The per-slot `dir`
cache was keyed on that pair, so with two live slots in one `Apply()` call the pair
collided: the first-processed slot inherited the *second's* commanded `dir` as its
guidance basis (a wrong missile-velocity model for λ̇, accumulating heading error),
while the second was permanently `continuous == false` (basis reset to the game's
own heading every frame). The iteration-order asymmetry is why exactly one of the
two missiles misbehaved.
*Fix*:
1. `SlotCacheEntry` is now keyed by the slot ROW index (0..15) — unique per live
   missile; the existing frame-continuity guard covers row re-use by a later
   missile. Steering math and write surfaces are unchanged.
2. Diagnostic counter `ownCol=` in the 2 s summary counts live slots that share one
   owner pair in a single frame; >0 proves the collision occurred and is now
   harmless (observation only).
3. ~~Terminal quiet zone at `range < 30 m`~~ — **removed; now opt-in `TerminalZone`**.
   It was added alongside this bug as a second, defensive guard, on the theory that the
   λ̇ demand saturates near the target and makes the missile orbit the impact point.
   That theory does not hold, and it was never the reported cause:
   * `ComputePn`'s `rmin` guard already floors `r²` at `max(2·|v|, 25 m)`. For a real MSL
     (~16.7 m/frame) that is **33.3 m**, so the guard engages *before* the old 30 m
     hand-back ever did. `pnsim` (k13c) asserts `|λ̇| ≤ |Vr|/rmin` inside the guard.
   * The rate limit bounds the achievable turn radius to `|v|/ω` — about **318 m** at the
     default 180 °/s — so orbiting a point 30 m away is geometrically impossible.
   * A closed-loop sweep over **972 engagement geometries** yields identical hit rate and
     closest approach with and without the hand-back (the sweep is the `run`
     comparison in `docs/HANDOFF.md` §11.11 discussion; the invariant is pinned by
     `pnsim` (k13a–k13d)).
   So PN now owns the entire flight: `[runtime] TerminalZone = 0` is the default. A
   positive value restores the hand-back for A/B comparison only; the `terminal=` counter
   in the 2 s summary reports frames spent inside the zone (0 unless opted in).
   The slot cache entry is still updated on every skip path, so a target that pulls away
   resumes the PN override cleanly on the next frame.

*Verify in-game*: set `[runtime] GuidanceOverride = 1` / `LogLevel = 2` in
`ACZero\ACZPropNav.ini` (no rebuild needed; the old `enable_pn.cmd` one-click has been
removed), fire two MSLs at one
target and check the 2 s summary line: `ownCol=` (>0 confirms the shared-owner
mechanism), `terminal=0` (full PN: the hand-back is off); both missiles should fly
smooth, matching trajectories.
