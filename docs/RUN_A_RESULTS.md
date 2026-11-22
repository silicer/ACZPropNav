# RUN_A_RESULTS — first read-only in-game probe (2026-10-26 21:14–21:18)

Run: one Free Mission sortie, 10 plain MSLs fired by the user (first and third shot had
the target re-locked while the missile was in flight). Build: probe build with
`Probe = 1`, `GuidanceOverride = 0`, `RingSurvey = 0`. Log: `ACZero\ACZPropNav.key.log`
(copy kept at `work/probe_a_run1.key.log`), 126 KB, no rotation.

Game image base at runtime: **`0x7FF650610000`** (ASLR is on; all probe-hook RVAs resolved
relative to the module base, so the run is still valid).

## 1. The decisive result: the flat `.wpb` record P was captured and validated live [V]

The first plain-MSL spawn (node pop caller `exe+0x13D703`, i.e. inside the known
`0x13D665` spawn site in `0x13D180`) produced:

```text
node = 0x2A00140DAB0   node+0x28 = P = 0x2A000C34B20
```

Dump at P (read-only) — **exactly the predicted flat record**:

| P offset | value | `.wpb` field | record offset |
|---|---|---|---|
| P-0x10 | 0x10 | `data_off` | 0x00 |
| P+0x00 | 0 / 0 / 0 / 1.0 | altMin / altMax / radius / explPers | 0x10 |
| **P+0x10** | **1600.0** | scope | 0x20 |
| **P+0x14** | **3000.0** | range | 0x24 |
| P+0x18 | 1.0 | hitRadius | 0x28 |
| P+0x1C | 0.0 | timeStart | 0x2C |
| P+0x20 | 4.0 | timeWait | 0x30 |
| P+0x24 | 4.0 | timeStep | 0x34 |
| P+0x28 | 0.0 | timeContinue | 0x38 |
| **P+0x2C** | **40.0** | lead | 0x3C |
| P+0x30 | 0.0 | dispersionAng | 0x40 |
| P+0x34 | 9.8 | accFall | 0x44 |
| P+0x38 / +0x3C | 0 | unknown0/1 | 0x48/0x4C |
| P+0x40 | 0 | unknown2 | 0x50 |
| **P+0x44** | **1000 (u32, u16)** | speed | 0x54 |
| **P+0x48** | **2** | type | 0x58 |
| **P+0x49** | **40** | attack | 0x59 |
| **P+0x4A** | **90** | startAng | 0x5A |

`docs/WEAPON_MODEL_LAYOUT.md` predicted all of these; this run turns the whole table into
**[V] for the live `mis` record**. `P` is a normal heap block (loaded by the name-keyed
asset loader), i.e. writable, and the pointer at `fire+0x1FB8` is the same object for the
player aircraft.

The probe's own consistency check printed `rec VALIDATED (flat .wpb record, variant=mis)`
and skipped the WeaponModel-scan / R3 passes for the rest of the run.

### Other PARAM pointers captured (not `mis`)

The run also dumped 7 other unique `node+0x28` pointers (8 total). Only the first one was
`mis`. One of them (from another `0x13D665` spawn) held a record with `scope=4000`,
`range=4000`, `lead=16`, `speed=1299`, `startAng=40`; the others had `data_off=0x10` but
non-`mis` values. None of the 10 `.wpb` files in `a.arc` has those numbers, so those
records are **other weapon records** (different aircraft/other `.arc` data) — not a
problem for the player-MS​L work, and a reminder that the pool-pop hook sees every spawn
(player, wingman, enemies).

## 2. WeaponModel allocation [V]

```text
ALLOC caller=exe+0xC767D size=0xD08 (3336 B) -> payload = 0xCC0  (WeaponModel vftable verified)
ALLOC caller=exe+0x16E14B size=0xA8940 (690496 B)               (mission singleton, 0x1B87B0 world)
```

So the typed `WeaponModel` payload for this mission is **0xCC0** bytes (the typed,
descriptor-driven serialization — not the 80-byte flat record). Only one such allocation
was captured; its payload was **not** in the probe's vtable-scan dump (the scan dumped 8
objects but not that pointer), so the typed layout is still an open item for a future
run (Run B captures it).

## 3. Plain-MSL spawn count and pool behaviour

* 8 `spawn@0x13D665` node pops (node `+0x28 = P`) — the user fired 10; the other 2 either
  came through a different path or were outside the captured window (the pop-event ring
  overflowed later in the run, after these 8).
* 64 `POP` log lines total. Most were `caller=exe+0x1C70C8 spawn@0x1C7080`, i.e. the
  generic effect/visual spawn path, **not** a missile.
* The 8 nodes that the sampler caught (before the first MSL was fired) were all state
  `+0x6B = 7` with world-like positions at `+0x20` (`[-1.954e4, 0, 4.804e4]` etc.) and
  `changed=0` over all 12 samples — static effect anchors, consistent with
  `docs/MSL_ADVANCE.md`'s "pool node has no per-frame writer".
* Singleton snapshot example (log line 435):
  `S=0x7FF6509319A0  L1[+20]=0x2A004009E80/+28=0x2A003FFBD30 n90=518  L2[+98]=0x2A00140E220/+A0=0x2A00140E0D0
   free3[+1A0]n210=411270 free4[+218]n288=8188  L5[+290] empty  cached+318=0x2A004009E80`.
  (`n90` etc. are the count fields the probe reads; their exact meaning is still
  uncertain — do not act on them.)

## 4. The world-tick hook did **not** fire (open issue)

`exe+0x1B7B00` was patched successfully (`stole 15 bytes`) but its detour was **never
entered** during the whole run (`g_world` stayed null; no `PROBE-A: WORLD` lines). The
alloc and pop hooks did fire normally, so the hook mechanism itself is fine.

Hypotheses: the per-frame call at `0xD1F05` targets a different entry (e.g. `0x1B7B90`),
or that update phase is not executed in this game mode. This is being verified statically;
**Run B does not depend on it** — mission control is reachable from the static global
`*(u64*)(base + 0x3409F8)` (same object re-fire documented as `MC`, also `*(S+0x428)`).

## 5. Other observations

* `PROBE-A: GLIST head=0x7FF65094EDB0 empty` for the whole run: the global list at
  `exe+0x33EDB0` is **not** the active projectile list in this mode. Ignore it.
* R3 anchored-delta tally ran (2032 MB, complete): 1600.0f = 128 hits, 3000.0f = 87,
  2000.0f = 284, 40.0f = 442, 80.0f = 296, 1000.0f = 582, i32 1000 = 1139, 90.0f = 59,
  u16 1000 = 12096, u16/u8 90 = 65536 (truncated). Top `(1600→3000)` deltas were small and
  coincidental; the validated P made this pass redundant. Kept for reference.
* The user's target re-lock on shots 1 and 3 cannot be seen in this run's data (no fire-
  object / lock-pair dump). Run B logs the lock pair and the missile's own state changes.

## 6. Conclusions and what Run B must answer

1. **The parameter side is solved**: the player's plain MSL carries `P` at
   `fire+0x1FB8` / `node+0x28`, and the offsets of lead/scope/range/startAng/speed are
   verified live and writable.
2. **Still unknown:** whether the plain-MSL guidance reads `lead` / `startAng` at all.
   Static analysis found the only reader of `P+0x2C` to be the `WeaponExSodCtrl` guidance
   law (`0x9AFB0`); if the MSL never reads it, a pure data patch (AC8 style) is impossible
   and PN must be a per-frame code override.
3. **Run B (implemented next)**, with no dependence on the broken world hook:
   * read `MC = *(u64*)(base+0x3409F8)`, then `playerPart -> fire -> P` (validated as here);
   * hardware one-shot read/watchpoints on `P+0x2C` (lead), `P+0x48` (type/attack/startAng),
     `P+0x14` (range), `P+0x44` (speed), per-thread, logged as `PROBE-B: HIT …`;
   * a per-250 ms object/part motion snapshot from `MC` (top movers) to identify the
     flying missile object/part (needed by the `pn_tick` fallback);
   * the same pool-node sampling for nodes spawned by `exe+0x13D703`, so the MSL node's
     transform can finally be watched while flying.
4. If Run B shows **no reader of `P+0x2C`/`P+0x4A`** during flight, the data route is dead
   for plain MSL: implement `pn_tick` (`docs/MSL_ADVANCE.md` §11.4) — hard-override
   `part+0x258`/`+0x268` every frame from the previous game value, target read from
   `docs/MSL_FIRE_OBJECT.md` §6.
