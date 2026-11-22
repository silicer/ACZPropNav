# Vanilla missile & weapon parameters (extracted from the shipping game)

**Source of truth:** extracted by us from the *shipping* Steam build,
`ACZero/a.arc` → `data/mis/mpl/*.wpb` / `*.wpt`, via
[`../tools/arc/arc_tool.py`](../tools/arc/arc_tool.py).
Nothing here comes from the community mod; the mod was only used as a *differential
control* (see §5) to prove which fields are live.

**Confidence vocabulary** used throughout: **HIGH** = directly read from shipping bytes and
corroborated by an independent second file; **MEDIUM** = read from shipping bytes but the
semantic is inferred; **LOW** = inference / unresolved.

---

## 1. Executive summary

* Missile/ordnance parameters live in `a.arc` → `data/mis/mpl/` as **96-byte fixed
  records**, one record per file.
* There are exactly **10 shipped records**: 4 missile-family (`mis`, `misf`, `misf2`,
  `mis_old`) and 6 gun-family (`gun`, `gun1`, `gun2`, `gun3`, `gunf`, `gunf2`).
* **There is no per-aircraft and no per-special-weapon parameter table in the shipping
  game.** `data/datapack_jp.bin` (the asset registry) references `mis/mpl/mis.wpb` and
  `mis/mpl/gun2.wpb` **199 times each — once per playable aircraft** — and references
  `misf.wpb`, `misf2.wpb`, `gunf.wpb`, `gunf2.wpb` exactly once each. So every aircraft
  fires the *same* MSL record. Special weapons (QAAM/LAAM/HVAA/SAAM/4AAM/…) have **no data
  file at all**; they are code-side. This independently confirms what the Enhanced Gunplay
  author says in public comments (§7).
* **`mis.wpb` is the player's standard missile** and is the only record that matters for
  the MSL behaviour the mod reworks. Its comment string in `mis.wpt`, decoded from cp932,
  is **`自機用通常ミサイル` = "player aircraft standard missile"**. `misf` =
  `僚機用通常ミサイル` = "**wingman** aircraft standard missile".
* The record has a genuine **turn-rate parameter at `+0x3C`** and a **seeker cone
  half-angle at `+0x5A`** — these are the vanilla tail-chase behaviour controls.
  **There is no separate per-missile turn-rate field**: `+0x3C` *is* the turn rate
  (degrees per second; see §4.1 and the §6 correction).

## 2. Record layout — `*.wpb`, 96 bytes, one record per file

**Confidence: HIGH.** Derived from the `.wpt` text schema and then verified value-for-value
against five independent files (§3). `--compare`/`wpb_dump.py` implement this table.

```
off    type  name            shipping value (mis.wpb)   semantics
-----  ----  --------------  ------------------------   -------------------------------
0x00   u32   data_off        0x00000010 (=16)           offset of the first data field
0x04   u32   reserved0       0                          always 0
0x08   u32   reserved1       0                          always 0
0x0C   u32   reserved2       0                          always 0
0x10   f32   altMin          0                          min altitude constraint (0 = none)
0x14   f32   altMax          0                          max altitude constraint (0 = none)
0x18   f32   radius          0                          proximity/blast trigger radius
0x1C   f32   explPers        1.0                        explosion persistence
0x20   f32   scope           1600                       SEEKER ACQUISITION / LOCK RANGE  [KEY]
0x24   f32   range           3000                       MAX FLIGHT DISTANCE              [KEY]
0x28   f32   hitRadius       1.0                        hit sphere radius
0x2C   f32   timeStart       0                          guidance phase start offset      [KEY]
0x30   f32   timeWait        4                          guidance phase duration          [KEY]
0x34   f32   timeStep        4                          phase step duration              [KEY]
0x38   f32   timeContinue    0                          post-phase continuation
0x3C   f32   lead            40                         TURN RATE, degrees/second       [KEY]
0x40   f32   dispersionAng   0                          aim dispersion, radians (guns)
0x44   f32   accFall         9.8                        gravity / fall acceleration
0x48   f32   unknown0        0                          always 0 in all 10 shipped records
0x4C   f32   unknown1        0                          always 0
0x50   f32   unknown2        0                          always 0
0x54   u16   speed           1000                       initial speed, m/s               [KEY]
0x56   u16   unknown3        0                          always 0
0x58   u8    type            2                          1 = gun, 2 = missile
0x59   u8    attack          40                         damage
0x5A   u8    startAng        90                         SEEKER CONE / LAUNCH GIMBAL, deg [KEY]
0x5B   u8    unknown4        0                          always 0
0x5C   u32   pad             0                          always 0
```

* `[KEY]` = feeds the PN mod / signature scanner.
* The record is **not** `u16 listNum` + `u16 childNum` + fields. That pair belongs to the
  *text* schema (`# system` header of `.wpt`); the binary starts with `u32 data_off = 16`
  and 12 reserved bytes. See §5 for how they relate.
* 68 bytes of floats (`0x10`–`0x53` = 17 f32) + 12 bytes of packed tail (`0x54`–`0x5F`)
  = 80 bytes of data, + 16-byte header = 96.

### 2.1 The trailing `u8` order, resolved from actual bytes

The `.wpt` schema lists `type, dispersionAng, attack, startAng` — but **that is not the
binary layout**: `dispersionAng` is an `f32` at `+0x40`, and the three `u8`s are packed at
`+0x58..+0x5A` in the order **`type, attack, startAng`**. Proof, reading the little-endian
dword at `+0x58` of the shipped files and cross-checking against the `.wpt` values:

| file | dword `+0x58` | `type` @58 | `attack` @59 | `startAng` @5A | `.wpt` says | match |
| --- | --- | ---: | ---: | ---: | --- | --- |
| `mis.wpb` | `00005a28` → bytes `02 28 5a 00` | 2 | 40 | 90 | type 2, attack 40, startAng 90 | ✅✅✅ |
| `mis_old.wpb` | `001e3202` → `02 32 1e 00` | 2 | 50 | 30 | type 2, attack 50, startAng 30 | ✅✅✅ |
| `gun.wpb` | `00000801` → `01 08 00 00` | 1 | 8 | 0 | type 1, attack 8, startAng 0 | ✅✅✅ |
| `gunf.wpb` | `002d0801` → `01 08 2d 00` | 1 | 8 | 45 | type 1, attack 8, startAng 45 | ✅✅✅ |
| `misf.wpb` | `003c2802` → `02 28 3c 00` | 2 | 40 | 60 | type 2, attack 40, startAng **20** | ⚠️ startAng differs — see §5 |

## 3. Complete vanilla record table (all 10 records)

**Confidence: HIGH** (every number below is a direct little-endian float/u16 read of the
bytes in `a.arc`). Dumped with `wpb_dump.py`; machine-readable copy regenerable with
`python tools/arc/wpb_dump.py --json work/a/data/data/mis/mpl/*.wpb`.

| file | kind | comment (cp932) | type | speed | scope | range | hitR | altMin/Max | tStart | tWait | tStep | tCont | **lead** | disp | accFall | attack | **startAng** |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `mis.wpb` | **player MSL** | 自機用通常ミサイル | 2 | 1000 | **1600** | **3000** | 1.0 | 0 / 0 | 0 | 4 | 4 | 0 | **40** | 0 | 9.8 | 40 | **90** |
| `misf.wpb` | wingman MSL | 僚機用通常ミサイル | 2 | 1000 | 1600 | 2000 | 1.0 | 0 / 0 | 0.5 | 14 | 10 | 0 | **80** | 0 | 9.8 | 40 | **60** |
| `misf2.wpb` | wingman MSL (2) | — (no .wpt) | 2 | 1000 | 1600 | 2000 | 1.0 | 0 / 0 | 2.5 | 18 | 10 | 0 | **28** | 0 | 9.8 | 40 | **30** |
| `mis_old.wpb` | *unused/legacy* | *(empty)* | 2 | 1000 | 2500 | 4000 | 1.0 | 0 / 0 | 0 | 10 | 0 | 0 | **10** | 0 | 9.8 | 50 | **30** |
| `gun.wpb` | player gun | 自機用通常機関砲 | 1 | 4000 | 800 | 1000 | 1.0 | 0 / 0 | 0 | 0 | 0.1 | 0 | 0 | 0.3 | 9.8 | 8 | 0 |
| `gun1.wpb` | gun (small cal) | 自機用通常機関砲(小口径高連射) | 1 | 4000 | 800 | 1000 | 1.0 | 0 / 0 | 0 | 0 | 0.1 | 0 | 0 | 0.3 | 9.8 | 8 | 0 |
| `gun2.wpb` | gun (large cal) | 自機用通常機関砲(大口径低連射) | 1 | 4000 | 800 | 1000 | 1.0 | 0 / 0 | 0 | 0 | 0.15 | 0 | 0 | 0.3 | 9.8 | 12 | 0 |
| `gun3.wpb` | gun (large cal) | 自機用通常機関砲(大口径高連射) | 1 | 4000 | 800 | 1000 | 1.0 | 0 / 0 | 0 | 0 | 0.1 | 0 | 0 | 0.2 | 9.8 | 12 | 0 |
| `gunf.wpb` | wingman gun | 僚機用通常機関砲 | 1 | 4000 | 800 | 800 | 1.0 | 0 / 0 | 0 | 3 | 0.1 | 1.5 | 0 | 0.3 | 9.8 | 8 | 45 |
| `gunf2.wpb` | wingman gun (2) | — (no .wpt) | 1 | 4000 | 800 | 800 | 1.0 | 0 / 0 | 0 | 3 | 0.1 | 1.5 | 0 | 0.3 | 9.8 | 8 | 45 |

`explPers` = 1.0 on all ten; `unknown0..4` and `pad` = 0 on all ten; `data_off` = 0x10 on
all ten.

### 3.1 Which records the game actually loads

`data/datapack_jp.bin` reference counts (`strings` census):

| record | refs in datapack | interpretation |
| --- | ---: | --- |
| `mis/mpl/mis.wpb` | **199** | every playable aircraft's MSL |
| `mis/mpl/gun2.wpb` | **199** | every playable aircraft's gun |
| `misf.wpb` / `misf2.wpb` | 1 / 1 | wingman missile variants |
| `gunf.wpb` / `gunf2.wpb` | 1 / 1 | wingman gun variants |
| `mis_old.wpb`, `gun1.wpb`, `gun3.wpb` | **0** | shipped but **unreferenced** (dead data) |

**Confidence: HIGH** for the counts, **MEDIUM** for "dead data" (they may be indexed by
ordinal elsewhere — but no name reference exists).

## 4. Guidance-relevant field semantics (with evidence)

| field | off | belief | confidence | evidence |
| --- | --- | --- | --- | --- |
| `scope` | `+0x20` | seeker acquisition / lock-on range (m) | **HIGH** | only range-like field besides `range`; vanilla 1600 is a plausible ACZ MSL lock range; the Gunplay mod raises it to 3000 to extend lock; `mis_old` (a "longer reach" experiment) raises it to 2500 |
| `range` | `+0x24` | max flight distance (m) | **HIGH** | `range/speed` for `mis` = 3000/1000 = 3 s ≈ `timeWait`; the mod raises it 3000→6000 for a range mod |
| `lead` | `+0x3C` | **turn rate, degrees/second** (was: "lead / pursuit gain") | **HIGH** (in-game) | (a) the *only* missile record that trades reach against tracking (`mis_old`: scope +900, range +1000, but `lead` 40→**10**, `startAng` 90→**30**) is the textbook "longer range, worse homing" experiment; (b) wingman missiles get `lead` **80** (2× player); (c) the mod author raises `misf2`'s `lead` 28→**80** to fix a weak wingman missile (§5.1). Unit settled in-game: read per frame as `maxTurnPerFrame = lead·π/180 / fps`, i.e. deg/s. **Not** a foresight value — see §6. |
| `startAng` | `+0x5A` | seeker cone half-angle / launch gimbal (deg) | **HIGH** (identity) | `mis` 90 (wide, launches at any aspect), wingman 60/30, guns 0/45; `mis_old` 30 pairs with its weak `lead`; the mod normalises `misf2` 30→**60** alongside its `lead` fix (§5.1) |
| `timeStart` | `+0x2C` | delay before guidance engages (s) | **MEDIUM** | 0 for player MSL (immediate guidance), 0.5/2.5 for wingman, 2.0 for the `gunf` long-burst logic |
| `timeWait` | `+0x30` | duration of the guidance phase (s) → **lifetime-ish** | **MEDIUM** | 4 s for player MSL; wingmen 14/18 s; `mis_old` 10 s. Best candidate for a `LifeTime` mapping, but `range/speed` also bounds life. |
| `timeStep` | `+0x34` | sub-step / re-aim period (s) | **MEDIUM** | 4 for MSL, 0.1 for guns (rapid), 0.15 for `gun2` (slow-firing) — clearly a cadence |
| `timeContinue` | `+0x38` | extra time after the phase | **LOW** | 0 for all missiles, 1.5 for wingman guns |
| `speed` | `+0x54` | initial speed (m/s) | **HIGH** | 1000 for missiles, 4000 for guns (ACZ gun rounds), physically sensible |
| `attack` | `+0x59` | damage | **HIGH** | name + monotone with gun calibre (8 small, 12 large) |
| `type` | `+0x58` | 1 = gun, 2 = missile | **HIGH** | clean split across all ten files |

### 4.1 What controls *turn rate*? — the answer is `lead` (`+0x3C`)

**Original conclusion of this section (kept for the reasoning): there is no separate
turn-rate field in the weapon schema**, and the 17 floats are fully accounted for by the
`.wpt` names. That much held.

Where an angular rate *does* exist in this game's data is the **aircraft** flight model:
`data/mis/mpl/<ac>.pmb` (400 B) with text twin `<ac>.pmt`, whose `pitch`/`roll`/`yaw`
sub-blocks carry `rotRate`, `rotMin … rotMax`, `rotBuffet`, `revRate` in deg/s — e.g.
`a06e.pmt` has `pitch.rotRate = 5.0`, `pitch.rotMax = 30.555`, `roll.rotMax = 133.8`.
That is the airframe, not the missile.

**Conclusion (confidence: HIGH, settled in-game):** the missile's maximum rotation rate is
**the `lead` field at `+0x3C`, in degrees per second** — a data field, read per frame by
the plain-MSL guidance function as `maxTurnPerFrame = lead·π/180 / fps`. Practical
consequence for the mod: `MaxRotationAngle` is enforced in our own guidance loop (we
command a turn rate and rate-limit it), and must **not** be mapped onto a different
`.wpb` field. Do not let the signature scanner search for an *additional* per-missile
turn-rate float — `lead` is it.

## 5. Differential evidence: are the `.wpt` text twins authoritative?

`.wpt` = compiled-out developer text form. They give the field **names/types/order** but are
**not** kept in sync with the binary, so for values trust `.wpb` only.

`wpb_dump.py --compare`, shipped `a.arc` records:

**`mis.wpb` → `mis_old.wpb`** (the deliberately different "long range / weak tracking" copy):

| field | +off | `mis` | `mis_old` | Δ |
| --- | --- | ---: | ---: | ---: |
| scope | 0x20 | 1600 | 2500 | +900 |
| range | 0x24 | 3000 | 4000 | +1000 |
| timeWait | 0x30 | 4 | 10 | +6 |
| timeStep | 0x34 | 4 | 0 | −4 |
| **lead** | 0x3C | 40 | 10 | **−30** |
| attack | 0x59 | 40 | 50 | +10 |
| **startAng** | 0x5A | 90 | 30 | **−60** |

Only 7 of ~20 fields differ, and the pattern is exactly *more reach, less homing* — the
strongest single piece of evidence that `lead` + `startAng` are the guidance controls.

**`mis.wpb` → `misf.wpb`** (player → wingman): `scope` unchanged; `range` 3000→2000;
`timeStart` 0→0.5; `timeWait` 4→14; `timeStep` 4→10; **`lead` 40→80**; **`startAng` 90→60**.
Wingmen turn twice as fast but get a tighter start cone and a shorter reach.

**Known `.wpt`↔`.wpb` discrepancies** (confidence HIGH, all confirmed by direct byte read):

1. `mis.wpt` lists 16 fields (`listNum 17` incl. comment) and omits `explPers` **and**
   `accFall`; the binary *does* contain `1.0` at `0x1C` and `9.8` at `0x44`. `mis.wpt` is
   stale — do not derive offsets from it.
2. `misf.wpt` says `timeStart 2.0` but the binary has `0.5`; says `startAng 20` but the
   binary has `60`.
3. `misf2.wpb` / `gunf2.wpb` have **no** `.wpt` at all, yet sit perfectly in the layout.

So: **`.wpb` is the runtime truth; `.wpt` is stale documentation.** The field *names* are
trustworthy (they corroborate every offset); the field *values* are not.

### 5.1 Mod differential — what a working missile mod actually edits (strongest evidence)

The Enhanced Gunplay mod ships its own `.arc` archives (`01_ACZ_Enhanced_Gunplay_A5.arc`,
`…_A6.arc`) which our tool parses directly. Diffing their `mis/mpl/*.wpb` against the
shipping records shows precisely which fields a functioning mod-author treats as live:

| archive | record | fields changed vs vanilla |
| --- | --- | --- |
| A5 | `mis.wpb` | `scope` 1600→3000, `range` 3000→5000 |
| A6 | `mis.wpb` | `scope` 1600→3000, `range` 3000→**6000** |
| A6 | `misf.wpb` | `scope` →3000, `range` 2000→6000, `timeWait` 14→4, `timeStep` 10→4 |
| A6 | `misf2.wpb` | `scope` →3000, `range` 2000→5000, `timeWait` 18→4, `timeStep` 10→4, **`lead` 28→80**, **`startAng` 30→60** |
| A5/A6 | `mis_old.wpb`, `gun*.wpb` | unmodified (`mis_old` is dead data, confirming §3.1) |

Two conclusions, both **HIGH** confidence:

1. **The player MSL (`mis.wpb`) is extended purely by `scope` + `range`** — the author never
   touches `lead`/`startAng` there. All missile-level "improvement" in the community mod is
   *reach*, not *homing quality*. That is exactly the gap our PN mod fills, and it confirms
   `scope`/`range` as the reach controls.
2. **The only record where the author does touch homing is `misf2.wpb`**, and he raises
   `lead` 28→**80** (to match `misf`) and `startAng` 30→**60** (also to match `misf`). He is
   normalising a weak wingman missile up to the standard wingman values. That is the
   cleanest possible demonstration that **`lead` and `startAng` are the homing-quality
   fields** — and it raises their confidence from MEDIUM to **HIGH** for *identity*
   (the exact unit of `lead` was settled later in-game: deg/s).


## 6. Mapping to the AC8 mod's INI surface

`ACZPropNav.ini` mirrors `AC8PropNav.ini`, so each AC8 property needs a home. **This table
is a data-side identification only** — the shipped mod does not drive PN through these
fields (see the correction at the end of this section):

| AC8 property | ACZ field | off | confidence | note |
| --- | --- | --- | --- | --- |
| `HomingForesightAmount` | *(no equivalent)* | — | — | see the correction below |
| `MaxHomingAngle` | **`startAng`** | `+0x5A` | **HIGH** (identity) | degrees, **half**-angle; vanilla 90 (⇒ 180° full cone); the mod normalises `misf2` 30→60 alongside `lead` |
| `LockonRange` | **`scope`** | `+0x20` | **HIGH** | vanilla 1600 m; the community range mod raised exactly this |
| `LifeTime` | **`timeWait`**, bounded by `range/speed` | `+0x30` | **MEDIUM** | vanilla 4 s; `range/speed` = 3 s. Pick `timeWait` as primary and clamp by `range` |
| `MaxRotationAngle` | **`lead`** | `+0x3C` | **HIGH** (in-game) | degrees per second. Vanilla 40 (player), 80 (wingman), 10 (weak `mis_old`) — the 40 matches AC8's own vanilla `MaxRotationAngle` |

> **Correction (2026-11, after the in-game read-watch).** Earlier revisions of this page
> identified `lead` (`+0x3C`) as the ACZ home of `HomingForesightAmount`. That is wrong.
> The field is read per frame by the plain-MSL guidance function as
> `maxTurnPerFrame = lead·π/180 / fps` — it is the **turn-rate clamp**, i.e. the ACZ
> analogue of `MaxRotationAngle`, not a foresight/lead distance. Vanilla value 40 °/s
> equals AC8's vanilla `MaxRotationAngle` of 40 °/s, and `mis_old`'s "longer reach,
> weaker homing" experiment lowers it 40→10 (slower turning) exactly as that reading
> predicts.
>
> Consequence: **there is no data-field route to proportional navigation in this game.**
> PN has to be applied in the guidance loop, which is what the shipped mod does (see
> `docs/MSL_PN.md`). `scope`/`range`/`startAng`/`attack` remain genuinely useful data
> fields — the identification of `startAng` and `scope` above is unaffected.

## 7. Community-sourced cross-checks (not extracted by us)

Marked clearly as *community claims*, used only as corroboration:

* The Enhanced Gunplay author (**Gamingroach**) states that for the standard MSSL "all base
  stats … can be changed", that **special weapons** "are bundled into a separate binary
  archive that needs to be reverse engineered", and that "MSSL reload times are shared with
  almost all missile type special weapons". He also names the file grammar: "WPB files in
  the extracted archives is what you want to look into. Its in hexadecimal binary format but
  there are plain text WPT files that show uncompiled stats of weapons" and "Datapack_jp.bin
  is where assets loading registry is stored where you can assign which planes can have
  which weapons/guns".
  Source: [ACZ Enhanced Gunplay addon page, comments](https://www.moddb.com/games/ace-combat-zero-the-belkan-war/addons/acz-enhanced-gunplay-mod)
* The unpacking tutorial confirms the community toolchain is `python aczarc.py <arc>` with
  `numpy` + `lz4`, and that archives can be repacked.
  Source: [ACZ PC unpack/repack .arc tutorial](https://www.moddb.com/games/ace-combat-zero-the-belkan-war/tutorials/acz-pc-unpack-repack-arc)
* The `.wpt`/`.wpb`/`Datapack_jp.bin` claims above are **independently confirmed** by our
  own extraction, so they are promoted to HIGH confidence in §2–§3.

**Not found in any community source:** numeric turn rate / homing cone / life / range per
special weapon (QAAM, LAAM, HVAA, SAAM, 4AAM, 6AAM, 2AAM, SASM, XAGM). Consistent with our
own finding that no such data exists in the shipping archives — those parameters are code,
not data.

## 8. Reproduction

```sh
PY=python
T=ACZPropNav/tools/arc/arc_tool.py
W=ACZPropNav/tools/arc/wpb_dump.py

# 1. confirm the container and the checksum
python $T info ACZero/a.arc

# 2. extract only the weapon/aircraft parameter directory (67 MiB)
python $T extract ACZero/a.arc --grep '^data/mis/' --out ACZPropNav/work/a

# 3. decode records
python $W ACZPropNav/work/a/data/data/mis/mpl/mis.wpb
python $W --compare .../mis.wpb .../mis_old.wpb

# 4. the asset registry (proves per-aircraft sharing)
python $T extract ACZero/a.arc --grep 'datapack_jp\.bin' --out ACZPropNav/work/x
strings -n 4 ACZPropNav/work/x/data/datapack_jp.bin | grep -oE 'mis/mpl/[a-z0-9_]+\.wpb' | sort | uniq -c
```

## 9. What is still open

* **Runtime placement: answered — the flat record does exist in memory, but only via a
  pointer.** An in-game probe captured a live record `P` (reached as
  `fire+0x1FB8` / `node+0x28`) and validated it field-for-field against this table:
  `P = .wpb + 0x10`, `*(u32*)(P-0x10) == 0x10`, `scope`/`range`/`lead`/`speed`/`type`/
  `startAng` all matching. So the layout in §2 holds verbatim at runtime. What does *not*
  work is scanning writable memory for the raw 96 bytes — the engine also keeps a
  re-serialised, header-per-field copy, so a naive full-memory scan misses. The mod
  therefore does not scan for these records by default; see `docs/HANDOFF.md` §3.5.
* **Special-weapon behaviour remains code-only.** If the mod ever needs per-SPW values they
  must be found in the exe (or measured in game), not in data.
* `misf2.wpb` / `gunf2.wpb` / `gun1.wpb` / `gun3.wpb` / `mis_old.wpb` have no `.wpt` twin
  and no datapack reference; their runtime role is unknown (LOW confidence).
