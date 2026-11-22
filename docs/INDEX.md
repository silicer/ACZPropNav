# docs/ — index

This directory is the project's documentation for **users** and **future maintainers**.
It is deliberately small: during development it also held a reverse-engineering lab
notebook (≈8,500 lines of dated investigation logs, probe-run records and dead-end
write-ups). Those were removed once their conclusions were embodied in the shipped code and
summarised in `HANDOFF.md` / `MSL_PN.md`; the full history is in git, and every fact that
was still load-bearing was carried over (see "What was removed", below).

---

## Read these first (60 seconds)

| # | file | why this one |
|---|------|--------------|
| 1 | [`../README.md`](../README.md) | What the mod does, how to install it, how to configure it. That is all a player needs. |
| 2 | [`HANDOFF.md`](HANDOFF.md) | The maintainer page: status, what ships, the INI contract, the verified RVAs/slot layout, the environment traps, and **the routes already proven dead**. Bilingual (Chinese first). |
| 3 | [`MSL_PN.md`](MSL_PN.md) | The technical core: the two hooks (`exe+0x13ECA0` for plain MSL, `exe+0x90510` for special weapons), the slot layout, the PN math, the INI mapping, the log fields, and how to verify a fix in one in-game run. |

Then, by task:

* **Tuning the mod** → [`CONFIG.md`](CONFIG.md) (every INI key, per-property semantics,
  what is and is not wired) → [`CALIBRATION.md`](CALIBRATION.md) (the log files, what to do
  when the log says nothing was found, what to do after a Steam update).
* **Reading the PN numbers** → [`PN.md`](PN.md) (the law, the blend, the 198-engagement
  simulation, and an honest account of what the sweep does and does not prove).
* **Changing the code** → [`ARCHITECTURE.md`](ARCHITECTURE.md) (layering, module map, hard
  rules, the discovery contract) then [`AGENTS.md`](../AGENTS.md) (branch model, CI, test
  gate, commit rules).
* **Cutting a release** → [`RELEASING.md`](RELEASING.md).
* **Extracting game data** → [`ARC.md`](ARC.md) (the `.arc` container format and
  `tools/arc`) → [`VANILLA.md`](VANILLA.md) (the resulting vanilla missile/weapon numbers).

---

## Everything in `docs/`

| file | lines | what it is |
|------|------:|------------|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 245 | Design document: target game/build, why an injected DLL rather than a data edit, the layering diagram, the module map, the hard safety rules, the (opt-in) discovery contract, the guidance override, and the offline test inventory. |
| [`ARC.md`](ARC.md) | 225 | The `ACZero/*.arc` container format, fully decoded: XOR keystream with a 32-bit dword key, LZ4 data blocks, front-of-body index; verified by re-producing the game's own body checksum on every shipped archive. Also documents `tools/arc/arc_tool.py`. |
| [`CALIBRATION.md`](CALIBRATION.md) | 175 | Bilingual operator manual: the three log files and `LogLevel` semantics, the bug-report procedure, symptom → action table, `Probe`/`[vanilla]` calibration loop, what to do when a game update breaks the hook byte checks, and why `LeadScale`/`LeadMode` are not handling knobs. |
| [`CONFIG.md`](CONFIG.md) | 140 | Configuration reference: INI syntax and matching rules, the five AC8 properties one by one (units, defaults, what is *not* wired and why), `[enemy]` semantics, the `[runtime]` extension keys, and a "what do I change to get X" cheat sheet. |
| [`HANDOFF.md`](HANDOFF.md) | 321 | Maintainer handoff (Chinese-first): status table, what ships and how it is installed, the INI contract, the verified technical facts as tables (two hook targets, RVAs, slot layout, units/constants), environment traps, the dead-end list, the diagnostic switches, what is still unwired, and the external code-review verification record. |
| [`INDEX.md`](INDEX.md) | — | This page. |
| [`MSL_PN.md`](MSL_PN.md) | 376 | The shipped guidance: why `FUN_14013ECA0` is the target, the slot layout at `ctrl+0x158+i*0x58`, the hook mechanics (stolen bytes, deferred install, detour ABI), the full per-frame algorithm, the INI mapping, every log field, the one-run verification recipe, the orientation-takeover (Step 1B) status, the true-λ̇ PN math (Step 2), the limitations, and the two-missile slot-cache fix. |
| [`PN.md`](PN.md) | 480 | The guidance law itself: vanilla pure pursuit vs proportional navigation, LOS rate / closing speed / time-to-go / lead point, the seeker cone and turn-rate limits, the `N·λ̇` rate law, the angular blend, mapping onto `HomingForesightAmount`, and the `tools/pnsim` 198-engagement results (crossing/beam hit rate 38.9 % → 57.4 %). |
| [`RELEASING.md`](RELEASING.md) | 169 | Maintainer release runbook: what a release is (one zip, flat into `ACZero\`), what ships and what must never ship, the `tools/package_release.sh` flow, the pre-flight checklist, and the known traps. |
| [`VANILLA.md`](VANILLA.md) | 322 | The vanilla missile/weapon numbers extracted from the *shipping* `a.arc` via `tools/arc/arc_tool.py`: the 96-byte `*.wpb` record layout, all 10 shipped records, the field semantics with evidence, and the differential evidence from the community mod. Source of truth for "what does the game actually use". |

Per-document line counts are approximate and drift with edits; they are here to show the
relative weight of each page, not to be exact.

---

## Conventions you will meet in these pages

* **Addresses are RVAs** written as `exe+0x13ECA0`, i.e. `VA − 0x140000000`. All claims are
  for `ACZero\acz.exe` **4,046,008 B, SHA-256 `3802B0DC…`**. A Steam update moves
  everything, and the mod's hooks then refuse to install (it never patches blind).
* **Confidence tags** (mostly in `VANILLA.md`): **HIGH** = read directly from shipping
  bytes and corroborated independently; **MEDIUM** = read from shipping bytes, semantics
  inferred; **LOW** = inference / unresolved.
* **Languages are mixed on purpose**: `CALIBRATION.md`, `CONFIG.md` and `HANDOFF.md` are
  Chinese-first with English alongside (that is what the users and the maintainer read);
  `ARCHITECTURE.md`, `MSL_PN.md`, `PN.md`, `ARC.md`, `VANILLA.md`, `RELEASING.md` are
  English. Don't "fix" that.
* **`work/` is not public.** The RE tooling in `tools/re/` and the reproduction commands in
  `VANILLA.md` / `ARC.md` assume a local `work/` directory holding copies of the game
  executable and of extracted assets. It is gitignored — it will not be in a clone. You
  have to re-create it from your own game install (see `ARC.md` §4 and `HANDOFF.md` §9).

## Maintaining this index

Adding a file to `docs/`? Add a row above in the same commit.

Rule of thumb for what belongs here: **a user or a future maintainer must be able to act on
it.** Instruction-level proof of a dead end belongs in `HANDOFF.md`'s dead-end table as one
line ("tried X, it cannot work, here is why"), not in a new 900-line page. If a page is
mostly dated narrative, its conclusion should move into a surviving page and the page
should go.

## What was removed, and where it went

15 pages left `docs/` when v1.0.1 shipped: the reverse-engineering lab notebook. Nothing
load-bearing was lost. If you find an old link or remember a page that is not here, this is
where its conclusions went:

| removed page(s) | was | conclusions now live in |
|---|---|---|
| the foundational RE report | addon ABI, AOB patching, AC8PropNav parity table, `acz.exe` static analysis | `ARCHITECTURE.md` (ABI / entry points), `HANDOFF.md` §3 & §9 (RVAs, RTTI tooling), `PN.md` §4 (AC8 property semantics) |
| the RTTI inventory, the runtime-parameter report, the weapon-model layout proof | generated RTTI map, runtime parameter layout, proof the records are re-serialised | `HANDOFF.md` §3.3 (record `P` offsets) and §3.5 (why the flat scan is off by default); regenerate the RTTI map with `tools/re/rtti_scan.py` |
| the guidance disassembly report | slot field map, target source, integrator hook recipe, thunk bytes | `HANDOFF.md` §3.4 (special-weapon constants) and `MSL_PN.md` (the plain-MSL contract that superseded it) |
| the five plain-MSL investigation reports | the hunt for the plain-MSL target/advance, fire-object and lock storage, pool-node proofs, the dispatcher pass | `HANDOFF.md` §5 (dead-end table, with the RVAs that identify each dead end) and `MSL_PN.md` (the surviving design) |
| the two probe designs and their two run logs | read-only survey and hardware read-watch designs, plus what each run found | `HANDOFF.md` §6 (what every diagnostic switch does) — the code is still in `src/core/probe_*.{h,cpp}` |
| the delivery-machine verification log | toolchain, build sizes, per-suite assertion counts, the list of what could not be verified there | `ARCHITECTURE.md` (test inventory), `RELEASING.md` (the release gate), `HANDOFF.md` §4/§7 (the traps it recorded) |

The full text of every removed page is in git history up to the v1.0.1 commit.

