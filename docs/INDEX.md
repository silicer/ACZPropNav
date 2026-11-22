# docs/ — index

This directory is the project's design documentation **and** its reverse-engineering
lab notebook. It mixes two kinds of writing:

* **current** pages — describe the mod as it is built today (design, formats, how to
  install, how to calibrate);
* **historical evidence** pages — long, dated investigation logs that record *how* the game
  internals were found: instruction-level proofs, dead ends, probe runs. They are
  valuable archaeology (the reasoning is reproducible), but a conclusion in an old
  log may have been corrected by a newer one. The table below says which is which.

---

## Read these three first (60 seconds)

| # | file | why this one |
|---|------|--------------|
| 1 | [`ARCHITECTURE.md`](ARCHITECTURE.md) | What the mod is, why it must be an injected DLL (Steam-wrapped exe, `*.arc` is only data), and the module contracts. Read this before any code. |
| 2 | [`HANDOFF.md`](HANDOFF.md) | The maintainer handoff: current status table, exactly what ships, the INI contract that may not be broken, and **the routes already proven dead** — this is the page that saves you from redoing weeks of work. Bilingual (Chinese first). |
| 3 | [`MSL_PN.md`](MSL_PN.md) | The newest and hardest piece: the post-update hook at `exe+0x13ECA0` that gives the *plain* MSL (the missile every aircraft fires) its proportional-navigation steering. Slot layout, steering math, tests. |

If you only want to **install and configure** the mod, stop reading `docs/` and use:
[`../README.md`](../README.md) → [`CALIBRATION.md`](CALIBRATION.md) (the three logs and
the "found it / didn't find it" loop) → [`VANILLA.md`](VANILLA.md) (the vanilla numbers
the defaults are based on).

If you came for the **reverse-engineering methodology**, read
[`RE.md`](RE.md) (loader ABI, AOB patching, AC8 parity) → [`ARC.md`](ARC.md) (the
container format) → [`WEAPON_MODEL_LAYOUT.md`](WEAPON_MODEL_LAYOUT.md) (how to find
runtime parameter records without hard-coded heap addresses).

---

## Status legend

| tag | meaning |
|-----|---------|
| **current** | Describes the shipped code / current workflow. Safe to trust. |
| **reference** | Lookup data or a spec, generated or extracted; authoritative for what it covers. |
| **historical evidence** | Dated investigation log. Instruction-level quotes and negative results. Trust the [V] claims, not necessarily the conclusions: newer evidence pages may correct them. |
| **historical evidence** (part superseded) | Dated investigation log whose conclusions are still the best available, but specific sections were explicitly corrected later — the row says which page corrects it. |

## Everything in `docs/` (23 files, this index excluded)

| file | lines | status | what it is |
|------|------:|--------|------------|
| [`ARC.md`](ARC.md) | 225 | **current** | The `ACZero/*.arc` container format, fully decoded: XOR keystream with a 32-bit dword key, LZ4 data blocks, front-of-body index; verified by re-producing the game's own body checksum on all shipped archives. Also documents `tools/arc/arc_tool.py`. |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 180 | **current** | Design document: target game/build, why a runtime DLL instead of a data edit, layering (`loader/` proxy → `core` → `mem` discovery → `pn` guidance), and the contract each module owes the others. |
| [`CALIBRATION.md`](CALIBRATION.md) | 215 | **current** | Bilingual operator manual: the three log files, what "record byte scan failed" means, how to turn a failed discovery into a working one, and how to calibrate the optional true-PN hook. The first thing to read when the mod "does nothing". |
| [`GUIDANCE_ASM.md`](GUIDANCE_ASM.md) | 800 | **historical evidence** | Missile guidance static analysis on the unpacked image: missile-slot field map (`Ctrl+0x80+i*0x68`), where the target really comes from (index pair re-resolved every frame, no target pointer), and the PN hook recipe. Every claim tagged `[V]`/`[I]`/`[U]`. Still the best field map; predates the `0x13ECA0` hook. |
| [`HANDOFF.md`](HANDOFF.md) | 556 | **current** | Maintainer handoff (Chinese-first): one-line status table, deliverables and how to use them, the INI schema that must not break, dead ends already ruled out, and what the next session should do. Best single page for "where are we". |
| [`MSL_ADVANCE.md`](MSL_ADVANCE.md) | 996 | **historical evidence** | Ghidra 12.1.4 pass on the plain-MSL per-frame advance. Proves the 0x70-byte pool node is written once at spawn (so PN cannot go there), then walks the world object/part model (`part+0x258` = position) and the weapon-dispatcher call chain. Self-flags superseded parts in §11.5. |
| [`MSL_FIRE_OBJECT.md`](MSL_FIRE_OBJECT.md) | 346 | **historical evidence** | The shooter's fire-control object, the locked-target storage, and the plain-MSL launch/aim RVAs. **Corrects** `MSL_GUIDANCE.md` §3.3 and `MSL_TARGET.md` §3–§4 — read it instead of those sections. |
| [`MSL_GUIDANCE.md`](MSL_GUIDANCE.md) | 532 | **historical evidence** (part superseded) | Proves what the plain MSL is *not* (not a `WeaponEx*Ctrl`, never reaches the `0x90510` integrator, not a `WeaponModel`) and then finds where it is: the projectile-pool singleton at `exe+0x3219A0`. §3.3 corrected by `MSL_FIRE_OBJECT.md`. |
| [`MSL_PN.md`](MSL_PN.md) | 298 | **current** | The implemented plain-MSL PN hook on `exe+0x13ECA0`: why that function (Run B's 88 per-frame `lead` reads), the slot layout at `ctrl+0x158+i*0x58`, the per-frame displacement cache it rewrites, the pure-math steering core, and which tests cover it. |
| [`MSL_TARGET.md`](MSL_TARGET.md) | 485 | **historical evidence** (part superseded) | Where the plain MSL gets its target and how it advances. Its headline correction: the node's `+0x50..+0x53` bytes are **not** a target index pair (they are a scalar + bit flags copied from the fire state). §3–§4 corrected by `MSL_FIRE_OBJECT.md`. |
| [`PN.md`](PN.md) | 477 | **current** | The guidance law itself: vanilla pure pursuit vs proportional navigation, LOS rate / closing speed / time-to-go / lead point, seeker-cone and turn-rate limits, the `N·λ̇` rate law, how the blend maps onto `HomingForesightAmount`, and the `tools/pnsim` engagement numbers (crossing-target hit rate 38.9% → 57.4%). |
| [`PROBE_RUN_A.md`](PROBE_RUN_A.md) | 186 | **current** | Design/implementation notes for the read-only survey run (Run A): `Probe = 1` enables three observation points — pool allocator, pool-node pop, per-frame world tick. No game memory is written; only hook jumps are installed and restored. |
| [`PROBE_RUN_B.md`](PROBE_RUN_B.md) | 177 | **current** | Design notes for the hardware read-watch (Run B): x86-64 debug registers + the existing VEH instead of `PAGE_GUARD` (which crashed the game twice — see `VERIFICATION.md`). Explains exactly which 4-byte fields are watched and why. |
| [`RE.md`](RE.md) | 1195 | **historical evidence** | The foundational RE report: the ACZ community addon ABI (`version.dll` preloader, `AczAddonInit`), how reference addons find game code (AOB patterns, version guards), `acz.exe` static analysis, and an exact property/offset parity table against the AC8 `AC8PropNav` mod. |
| [`RTTI.md`](RTTI.md) | 457 | **reference** | Generated inventory of all 404 MSVC RTTI classes in the unpacked `acz.exe` with vtable/COL RVAs, grouped by family (weapon controllers, models, world objects). Regenerate with `tools/re/rtti_scan.py`. |
| [`RUNTIME_PARAMS.md`](RUNTIME_PARAMS.md) | 908 | **reference** | Runtime layout of the `.wpb` weapon-parameter records and — the useful part — how to *locate* those objects at runtime without hard-coded heap addresses. Starts with a validity statement pinning the exact exe SHA-256. |
| [`RUN_A_RESULTS.md`](RUN_A_RESULTS.md) | 133 | **historical evidence** | Results of the first in-game read-only probe (2026-10-26): captured the live `.wpb` record `P` from a pool-node pop and validated every predicted field against it. The "it worked in the real process" proof for the locator. |
| [`RUN_B_RESULTS.md`](RUN_B_RESULTS.md) | 125 | **historical evidence** | Results of the hardware read-watch (2026-10-26): `lead` (`P+0x2C`) is read **88 times per sortie by `FUN_14013ECA0`** — the finding that chose the target `MSL_PN.md` hooks. Also the negative results (which fields nobody read). |
| [`VANILLA.md`](VANILLA.md) | 302 | **reference** | The vanilla missile/weapon numbers extracted from the *shipping* `a.arc` via `tools/arc/arc_tool.py`: the 96-byte `*.wpb` record layout, all 10 shipped records, and the values the INI defaults come from. Source of truth for "what does the game actually use". |
| [`VERIFICATION.md`](VERIFICATION.md) | 336 | **historical evidence** | Delivery-machine verification record (bilingual): toolchain, build output sizes, what each offline test asserts and how many assertions, and an explicit list of what could **not** be verified there (the game was not launched on that machine). |
| [`WEAPON_DISPATCH.md`](WEAPON_DISPATCH.md) | 137 | **historical evidence** | First Ghidra pass on the per-frame weapon/projectile dispatcher `FUN_1401B9A80`: the time-ordered global list, the node layout, and how the state byte `+0x6B` selects the dispatch path. Superseded for the MSL by `MSL_ADVANCE.md`, still the clearest dispatcher overview. |
| [`WEAPON_MODEL_LAYOUT.md`](WEAPON_MODEL_LAYOUT.md) | 701 | **historical evidence** | Proves the flat `.wpb` records do **not** live inside a `WeaponModel` (its payload is descriptor-driven and ≥ 4×0x100 bytes), and that gameplay code reads `scope/range/lead/...` through a separate pointer at `fire+0x1FB8` (a 6-slot array at `+0x1F08`). |
| [`RELEASING.md`](RELEASING.md) | 170 | **current** | Maintainer runbook for a public release: the flat "unzip into `ACZero\"" zip layout, what ships, build + test on Windows, the pre-flight checklist, and the traps (INI BOM, quiet default `LogLevel`, unsigned binaries). Read it before tagging, not after. |

## How the MSL investigation fits together

The plain MSL (the missile every aircraft fires) is the one thing the game hides well.
The trail, in reading order:

1. [`RE.md`](RE.md) — loader/ABI groundwork; `MSL_GUIDANCE.md` — where the MSL *is*
   (a projectile pool at `exe+0x3219A0`), plus the list of things it is not.
2. [`RUNTIME_PARAMS.md`](RUNTIME_PARAMS.md) + [`WEAPON_MODEL_LAYOUT.md`](WEAPON_MODEL_LAYOUT.md)
   — the parameter records and how to find them at runtime.
3. [`MSL_TARGET.md`](MSL_TARGET.md) → [`MSL_FIRE_OBJECT.md`](MSL_FIRE_OBJECT.md) — the
   target read-path, and the correction that the pool node carries **no** target pair.
   Read `MSL_FIRE_OBJECT.md` when the two disagree.
4. [`MSL_ADVANCE.md`](MSL_ADVANCE.md) — what actually moves a missile (world object/part
   model, not the pool node) and the per-frame call chain.
5. [`GUIDANCE_ASM.md`](GUIDANCE_ASM.md) — the guidance field map and the hook recipe.
6. [`PROBE_RUN_A.md`](PROBE_RUN_A.md) + [`RUN_A_RESULTS.md`](RUN_A_RESULTS.md) — the
   locator works in the live process. [`PROBE_RUN_B.md`](PROBE_RUN_B.md) +
   [`RUN_B_RESULTS.md`](RUN_B_RESULTS.md) — `lead` is consumed per frame by
   `FUN_14013ECA0`.
7. [`MSL_PN.md`](MSL_PN.md) — the implemented fix.

## Conventions you will meet in these pages

* **Addresses are RVAs** written as `exe+0x1B9A80`, i.e. `VA − 0x140000000`. All claims
  are for `ACZero\acz.exe` **4,046,008 B, SHA-256 `3802B0DC…`** (see
  `RUNTIME_PARAMS.md` §"Validity statement"). A Steam update moves everything.
* **Confidence tags**: `[V]` verified at instruction/pseudocode level,
  `[I]` inferred from verified facts, `[U]` unknown with a concrete probe suggested.
  Treat `[U]` as a TODO, not as a fact.
* **Languages are mixed on purpose**: user-facing pages (`CALIBRATION.md`,
  `HANDOFF.md`, `VERIFICATION.md`) are Chinese-first with English alongside; the RE logs
  are English. Don't "fix" that.
* **`work/` is not public.** Many evidence pages cite paths like
  `work/acz_copy.exe.unpacked.exe`, `work/ga/full2.asm` or `work/rp/idx/`. That directory
  is the maintainer's local scratch space (copies of the game exe, extracted game
  assets, third-party downloads) and is gitignored — **it will not be in a clone**.
  Commands in the evidence pages therefore assume you have re-created `work/` yourself
  from your own game install with `tools/arc/arc_tool.py` and a SteamStub unpacker. The
  conclusions still stand; the quoted instructions are what matters.

## Maintaining this index

Adding a file to `docs/`? Add a row to the big table in the same commit. Generated
pages (`RTTI.md`) say how to regenerate; probe/run logs are historical — never edit
their conclusions in place, add a newer page and mark the old row
**superseded (part)**, like `MSL_FIRE_OBJECT.md` does.
