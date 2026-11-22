# Releasing ACZPropNav

How to turn a green source tree into a public release. The repo contains source only;
**binaries are never committed** — `build/` is gitignored and a GitHub source
download has no DLLs. Users get binaries from a Release asset built by this process.

---

## 1. What a release is

A release is **one zip attached to a git tag**, and its contents **extract straight
into the game folder** — no installer, no subdirectory:

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

```sh
# 0. clean tree, correct version
git status                     # must be clean
grep -n 'kVersion' src/core/mod.cpp       # bump it here, commit that first

# 1. build + run every offline test
./build.sh test                # preferred: runs all six test binaries
                               # (make test runs only initest/pnsim/memtest/hooktest)

# 2. package
tools/package_release.sh --version 1.0.0
#    → dist/ACZPropNav-1.0.0-win64.zip  + printed sha256 and member list
#    optional: --keep          keep the staging tree for inspection

# 3. tag + publish
git tag -a v1.0.0 -m "ACZPropNav 1.0.0"
git push origin v1.0.0
# create the GitHub Release for the tag and attach the single zip
```

Attach **one asset**: the zip. Do not also attach loose DLLs (users get confused about
which layout works) and do not commit `dist/` (gitignored).

### Where the binary must come from

The DLLs are Windows **x64** PE files. There are two legitimate build hosts:

* **Windows** with LLVM-MinGW/mingw-w64 — this is the verified path (`docs/VERIFICATION.md`).
* **Linux cross-compile**, which is what this checkout does by default
  (`x86_64-w64-mingw32-g++`). `./build.sh` produces the same kind of artefacts as the
  Windows build (same sources, same flags), but the offline test binaries are `.exe`
  files, so `./build.sh test` **cannot run them** without Wine. Options: run the tests
  on a Windows box before tagging, or run the step under Wine
  (`wine ./build/initest.exe …`), not just cross-compile and hope.

Do not "fix" the missing tests by skipping them: `initest`/`pnsim`/`memtest`/
`hooktest`/`hwwatchtest`/`smoke` are the only automated proof that the INI parser, the
PN math, the discovery fingerprints, the hook prologue matching and the real DLL
load path still work. A release with unrun tests is a release with no evidence.

## 5. Pre-flight checklist

Run through this immediately before tagging:

- [ ] `git status` is clean; `git status --ignored --short` shows `build/` and `work/`
      as ignored, **not** as untracked.
- [ ] `git ls-files | grep -E '^(work|build)/'` prints nothing.
- [ ] Version bumped in `src/core/mod.cpp` (`kVersion`), README/README.en mention the
      same version if they quote one, tag matches (`v1.0.0`).
- [ ] `./build.sh test` ends with `ALL TESTS PASSED` (on Windows or under Wine).
- [ ] `file build/ACZPropNav.dll build/XINPUT1_4.dll build/VERSION.dll` →
      `PE32+ executable ... x86-64 (DLL)` for all three. A 32-bit or wrong-arch
      artefact will fail to load and looks like "the mod does nothing".
- [ ] Proxy export tables still match the system DLLs (the load path breaks silently
      if an export is dropped). Count the EAT slots; expected **15** for
      `XINPUT1_4.dll` (8 named — `XInputGetState`, `XInputSetState`, … — plus
      ordinal-only forwarders, mirroring the system DLL's table shape) and **17**
      named entries for `VERSION.dll`:
      ```sh
      OBJDUMP=$(command -v x86_64-w64-mingw32-objdump || echo objdump)
      $OBJDUMP -p build/XINPUT1_4.dll | sed -n '/Export Address Table -- Ordinal Base/,/^$/p' | grep -c '+base'   # 15
      $OBJDUMP -p build/VERSION.dll   | sed -n '/Export Address Table -- Ordinal Base/,/^$/p' | grep -c '+base'   # 17
      ```
- [ ] `tools/package_release.sh --version <ver>` succeeds: it re-checks DLL
      architecture and the "no game files" rule for you.
- [ ] `unzip -l dist/ACZPropNav-<ver>-win64.zip` lists only the layout in §1; the
      sha256 the script printed is pasted into the release notes.
- [ ] Release notes state: game build tested (`SHA-256 3802B0DC…` / Steam build id),
      what changed, and that Steam updates may move the RVAs the mod patches.

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
