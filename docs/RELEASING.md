# Releasing ACZPropNav

How to turn a green source tree into a public release. The repo contains source only;
**binaries are never committed** — `build/` is gitignored and a GitHub source
download has no DLLs. Users get binaries from a Release asset built by this process.

---

## 1. What a release is

A release is **one zip attached to a git tag**, and its contents **extract straight
into the game folder** — no installer, no wrapper subdirectory. This is enforced:
`tools/package_release.sh` builds the zip from a flat staging dir and then refuses
to finish if any member path contains `/`. (v1.0.0 and v1.0.1 both shipped with an
`ACZPropNav/` wrapper, which put the proxy at `ACZero\ACZPropNav\XINPUT1_4.dll`
where the game never loads it — the documented install silently did nothing.
`./build.sh test` now runs the same flat-layout check so it cannot recur.)

```
<Steam>\steamapps\common\ACE COMBAT ZERO THE BELKAN WAR (Steam Version)\
'- ACZero\                      <- user unzips the release HERE, next to acz.exe
   |- acz.exe
   |- ACZPropNav.dll            (the mod)
   |- XINPUT1_4.dll             (proxy loader)
   |- ACZPropNav.ini            (configuration)
   |- README.md / README.en.md / LICENSE / RELEASE_NOTES.md
   '- ACZPropNav.log            (created at runtime)
```

Install = unzip into `ACZero\`. Uninstall = delete the two DLLs. Because there is no
script, there is no `build/` subfolder requirement and no `%ROOT%\..\ACZero` path
assumption to satisfy — the whole old `install.cmd` layout was retired on purpose
(it was the main source of "I unzipped it in the wrong place" reports).

`tools/package_release.sh` builds exactly this tree; do not hand-assemble it.

## 2. What ships

| file | what it is | required |
|------|------------|----------|
| `ACZPropNav.dll` (from `build/`) | the mod (exports `AczCoreRun` + community `AczAddonInit`) | yes |
| `XINPUT1_4.dll` (from `build/`) | XInput proxy loader, forwards to System32 | yes |
| `ACZPropNav.ini` | default config, **taken from the repo root** | yes |
| `README.md`, `README.en.md` | user docs | yes |
| `RELEASE_NOTES.md` | what changed in this version | yes |
| `LICENSE` | MIT + NOTICE (no game data; vendored script excluded) | yes |

`VERSION.dll` (the alternative `version.dll` proxy) is **built but not shipped**: it only
matters to users who already run the community loader, and those users copy
`ACZPropNav.dll` into `ACZero\mods\` instead. It stays in `build/` for them.

## 3. What must NEVER ship

* **`work/`** — every file in it, without exception: `work/acz_copy.exe` and
  `work/acz_copy.exe.unpacked.exe` (copies of the game executable, one of them
  SteamStub-decrypted), extracted game assets (`work/a`, `work/a5arc`, `work/a6`, …),
  the "ACZ Enhanced Gunplay" community mod download (`work/egp`, `work/dl`), the
  Steamless unpacker (`work/tools/`), and the RE dumps (`work/rp`, `work/ga`, …).
  Publishing any of it is a copyright violation and the unpacker/mod archives are
  third-party code you have no license to redistribute.
* **`build/obj/`**, `*.o`, `*.d`, `*.exe` test binaries, and the smoke-test scratch
  directories (`build/smoke_iso/`, `build/smoke_*/`, `build/sp*/`) together with the
  `*.log` files inside them — build scratch, not a deliverable. They also contain this
  machine's absolute paths.
* **Any game file at all**: `acz.exe`, `*.arc`, `*.wpb`/`*.wpt`, saves. The mod loads
  beside the game and reads the user's own install; it never bundles it.
* **The vendored script** `tools/arc/upstream/aczarc.py` (see `LICENSE` §3): it is not
  covered by the MIT grant, and nothing shippable needs it.

`tools/package_release.sh` enforces this by building from an explicit allowlist (not by
copying a directory) and then re-reading the finished zip: an unexpected member path,
a forbidden extension, or a non-`PE32+ x86-64` DLL aborts the run.

## 4. Cutting the release

**It is automated.** You do not tag or upload anything by hand; merging `dev` into
`main` publishes. See `AGENTS.md` for the branch rules.

```sh
# 1. on dev: green CI and a decided version
git switch dev
git status                              # must be clean
grep -n 'kVersion' src/core/mod.cpp     # bump it here if this is a version bump
#   CI (.github/workflows/ci.yml) must be green on dev

# 2. update RELEASE_NOTES.md — it becomes the body of the GitHub Release verbatim
$EDITOR RELEASE_NOTES.md

# 3. open a PR dev -> main (or let the maintainer merge) and merge it
#    the release workflow then:
#      builds on Windows + runs the full offline suite
#      reads kVersion -> packages ACZPropNav-<ver>-win64.zip
#      creates tag v<ver> if absent
#      creates (or updates) the GitHub Release with the zip attached
```

Manual fallback, if CI is unavailable:

```sh
./build.sh test                         # full suite, on Windows
tools/package_release.sh --version X.Y.Z --out-dir dist
gh release create vX.Y.Z dist/ACZPropNav-X.Y.Z-win64.zip \
   --title "ACZPropNav X.Y.Z" --notes-file RELEASE_NOTES.md
```

Attach **one asset**: the zip. Do not also attach loose DLLs (users get confused about
which layout works) and do not commit `dist/` (gitignored).

**Re-releasing the same version** is a deliberate act, not something an ordinary
merge does. A push to `main` only publishes when `kVersion` names a version without a
tag; otherwise the workflow builds, tests and stops (and says so in its summary).
To republish an existing version on purpose - the repair path for a bad build - run
the release workflow manually (`workflow_dispatch`). Bump `kVersion` whenever the
binaries change.

### Where the binary comes from

The DLLs are Windows **x64** PE files. Release builds are produced by CI on a native
`windows-latest` runner (`.github/workflows/release.yml`), which also runs the full
offline suite before publishing — that is the authoritative path.

A local build is fine for development. Either:

* **Windows** with LLVM-MinGW/mingw-w64 — the full suite runs natively.
* **Linux cross-compile** (`x86_64-w64-mingw32-g++`, the default here). `./build.sh`
  produces equivalent artefacts (same sources, same flags), but the test binaries are
  `.exe` files, so `./build.sh test` **cannot execute them** without Wine. It detects
  this and runs the pure-math `pnsim` natively instead, clearly saying the rest was
  skipped. That is a reduced gate: if your change touches hooks, the memory layer or
  the loader, let CI (or a Windows box) run the full suite before you merge to `main`.

Do not "fix" the missing tests by skipping them: `initest`/`pnsim`/`memtest`/
`hooktest`/`hwwatchtest`/`smoke` are the only automated proof that the INI parser, the
PN math, the discovery fingerprints, the hook prologue matching and the real DLL
load path still work. A release with unrun tests is a release with no evidence.

## 5. Pre-flight checklist

The workflow enforces most of this automatically (build + full test suite + zip
verification happen before anything is published), so the list is short on purpose.
Check these **before merging `dev` into `main`**:

- [ ] `git status` is clean; `git ls-files | grep -E '^(work|build)/'` prints nothing.
      (`work/` holds copies of the game executable — never commit it.)
- [ ] CI green on `dev`.
- [ ] `kVersion` in `src/core/mod.cpp` is the version you intend to release. It is the
      **only** place the version lives; the packager and the workflow both read it.
- [ ] `RELEASE_NOTES.md` describes this release — it becomes the GitHub Release body
      verbatim, with the build's sha256 appended automatically.
- [ ] If you changed the loader: the proxy export tables still match the system DLLs
      (a dropped export breaks the load path silently). Expected EAT slots: **15** for
      `XINPUT1_4.dll`, **17** for `VERSION.dll`:
      ```sh
      OBJDUMP=$(command -v x86_64-w64-mingw32-objdump || echo objdump)
      $OBJDUMP -p build/XINPUT1_4.dll | sed -n '/Export Address Table -- Ordinal Base/,/^$/p' | grep -c '+base'   # 15
      ```
- [ ] If this is the first release against a new game build: say so in the notes. A
      Steam update can move the RVAs the mod patches; the hooks then refuse to install
      (logging why) rather than mis-patching, so the mod degrades to vanilla instead
      of crashing — but it does stop working until the RVAs are re-verified.

## 6. Known traps (they have bitten before)

* **`ACZPropNav.ini` must stay valid for the shipped parser.** The INI contract is in
  `HANDOFF.md` §1 (`[player]`/`[enemy]`, AC8 property names, blank = vanilla) and is
  asserted by `initest`. Changing a key name is a breaking change.
* **Keep the INI's UTF-8 BOM.** The shipped file starts with `EF BB BF` and its comments
  are Chinese; re-saving it as BOM-less UTF-8 or as GBK garbles the comments in some
  editors. `.gitattributes` marks it `text`, and the parser tolerates CRLF and a missing
  BOM, but the canonical file has the BOM — `package_release.sh` warns if it does not.
* **Uninstall leaves the INI and the logs** in the game folder by design (there is no
  uninstaller any more; the user deletes the two DLLs). Say so in the release notes.
* **No code signing.** Windows SmartScreen may warn about the zip/DLLs. Do not add a
  self-signed "fix"; document the sha256 instead.
* **The default `LogLevel` is 1 (quiet).** A "no log file content" bug report is normal
  unless the reporter set `LogLevel = 2`. The README's issue section asks them to.
* **No CI workflow is defined.** Compiling is easy to automate
  (`apt install g++-mingw-w64-x86-64 && ./build.sh`), but the tests execute Windows
  binaries: on Linux without Wine, `./build.sh test` compiles and runs the native
  `pnsim --selftest` only and says so explicitly. Until a CI is set up deliberately, the
  `./build.sh test` + tag procedure above is the gate.

## 7. After the release

If a bad build goes out: do **not** move or delete the tag (people and caches already
point at it). Publish a new patch version, say clearly in the notes that the previous
asset is broken, and mark the old release as superseded. The mod's failure mode matters
here: a wrong-architecture or stale DLL loads, does nothing, and leaves nothing in the
log beyond the version banner — so tell users to check the banner line in
`ACZero\ACZPropNav.log` before reporting.
