# AGENTS.md — how work happens in this repository

This file is the contract for anyone (human or AI agent) changing this repo.
Read it before your first commit.

---

## 1. Branch model

```
main   ← always releasable. Only ever advanced by merging `dev`.
└─ dev ← all development happens here.
```

* **Work on `dev`, never on `main`.**
* `main` is protected by convention and by this workflow: a release is published
  the moment anything lands on `main` (see `.github/workflows/release.yml`), so a
  stray commit to `main` is a stray release.
* The maintainer merges `dev` → `main` **only after confirming the change in-game**
  or otherwise signing off. Never merge your own work into `main` to "save a step";
  open the PR and let the maintainer decide.

### Day-to-day

```sh
git switch dev
git pull --ff-only
# ... make changes ...
git add -A && git commit            # commits are SSH-signed; keep them focused
git push origin dev
```

### Getting a change into `main`

1. Push the work to `dev` and make sure CI (`.github/workflows/ci.yml`) is green.
2. Open a pull request **`dev` → `main`** and describe what was verified and how
   (in-game run, offline tests, both).
3. The maintainer reviews and merges. That merge triggers the release workflow.
4. Do not delete `dev`; it is the long-lived integration branch.

A direct `git merge dev` into `main` by the maintainer is equally valid — the
release workflow keys off "a push to `main`", not off the PR mechanism.

---

## 2. CI and release

| workflow | trigger | what it does |
|---|---|---|
| `.github/workflows/ci.yml` | push to `dev`; PR targeting `main` | builds on native Windows, runs the **full** offline suite, verifies the release zip builds. A second Linux job proves the cross-compile path and runs the math self-test natively. |
| `.github/workflows/release.yml` | push to `main` (i.e. a merge) | builds + tests again (same gate), reads the version from `kVersion`, packages the zip, tags `v<version>` if absent, then creates or updates the GitHub Release with the zip attached. |

Rules that follow from this:

* **Never edit the version by hand in more than one place.** The single source of
  truth is `kVersion` in `src/core/mod.cpp`. CI and the packager both read it.
* **Never push to `main` directly.** It publishes.
* **A red CI is not a reason to re-push `main`.** Fix it on `dev`.
* Bumping the version is a normal commit on `dev`; it becomes a release when merged.

---

## 3. Building and testing

The toolchain is MinGW-w64 (any `x86_64-w64-mingw32-g++`). Everything is driven
by `build.sh`; there is no configure step.

```sh
./build.sh          # build all targets (stripped release binaries)
./build.sh test     # build + run every offline test  ← the gate
./build.sh debug    # unstripped -O1 -g into build/debug/ (crash analysis)
./build.sh clean
```

`./build.sh test` is environment-aware: on a host that cannot execute Windows PE
files (Linux without Wine) it says so explicitly and instead compiles/runs `pnsim`
natively for the guidance math. **That is a reduced gate.** If your change touches
anything Windows-specific (hooks, memory layer, loader), you need the full suite —
run it on Windows, or let CI run it.

### What must stay green

* `pnsim --selftest` — the guidance math (69 assertions). Pure, no Win32.
* `initest`, `memtest`, `hooktest`, `hwwatchtest`, `smoke` — Windows PE tests.
* `tools/package_release.sh` must still produce a valid zip.

**Add a regression test with every bug fix.** The maths layer is deliberately
`pnsim`-testable; a fix without an assertion tends to come back.

---

## 4. Things that must never enter the repository

* **Game data of any kind**: `acz.exe`, any `*.arc`, `*.wpb`/`*.wpt`, saves.
  The mod loads beside the user's own install; it redistributes nothing.
* **Reverse-engineering scratch**: `work/` is gitignored on purpose. It holds copies
  of the game executable (one SteamStub-decrypted), extracted game assets,
  third-party mod downloads and an unpacker. Committing anything from it is a
  copyright problem and, for the binaries, a redistribution problem.
* **Built binaries**: `build/`, `dist/`. Releases carry the DLLs; the repo does not.
* **`tools/arc/upstream/aczarc.py`** is vendored community code under an unknown
  licence — it is excluded from the MIT grant and from release zips (see `LICENSE`).

---

## 5. Code conventions worth knowing

* C++17, `-fno-exceptions -fno-rtti`, no allocation in hot paths. The detours run on
  the **game thread every frame**; `Tick()` runs on a worker thread. Anything shared
  between them is `volatile` + `Interlocked*`, or write-once with a release flag.
* **Never log from inside a detour.** Publish into fixed arrays / a detail ring and
  let `Tick()` drain it.
* **Never trust a game pointer.** All reads go through `mem::Read*` / `IsReadable`
  (VEH-guarded); hooks validate the target function's entry bytes at runtime and
  refuse to install on a mismatch.
* Guidance maths belongs in `src/pn/msl_steer.h` (pure, header-only, `pnsim`-tested)
  — not in the detour.
* Logging: `Info()` = startup/status (visible at the default `LogLevel=1`),
  `Key()` = verbose telemetry (needs `LogLevel>=2`), `Warn()`/`Error()` = problems.
  Do not put per-frame output behind `Info()`.

---

## 6. Documentation

* `README.md` / `README.en.md` — user-facing only. Keep them short; anything that is
  implementation detail belongs in `docs/`.
* `docs/INDEX.md` — the map. Update it when you add or remove a doc.
* `docs/HANDOFF.md` — maintainer handoff: status, verified facts, environment traps,
  and **which approaches were tried and rejected**. If you spend real time proving
  something cannot work, record it there so nobody repeats it.
* `docs/RELEASING.md` — the release runbook (the CI automates most of it now).

---

## 7. Checklist before you push

- [ ] `./build.sh test` passes (full suite if you touched Windows-specific code)
- [ ] A regression test exists for any bug you fixed
- [ ] No game data, no `work/`, no `build/` staged (`git status` is clean of them)
- [ ] Version untouched unless this is a deliberate release bump (`kVersion` only)
- [ ] Docs updated: `docs/INDEX.md` if files moved, `HANDOFF.md` if you learned
      something a future maintainer needs
- [ ] You are on `dev`, and you are not pushing to `main`
