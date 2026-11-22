#!/usr/bin/env bash
# =============================================================================
# ACZPropNav - assemble a clean, publishable release zip.
#
#   tools/package_release.sh [--version X.Y.Z] [--out-dir DIR] [--keep] [--help]
#
# Produces  <out-dir>/ACZPropNav-<version>-win64.zip  whose contents extract
# DIRECTLY into the game folder (`ACZero\`, the directory containing acz.exe):
#
#   ACZero\
#   |- ACZPropNav.dll      the mod
#   |- XINPUT1_4.dll       loader/proxy (forwards to System32\XInput1_4.dll)
#   |- ACZPropNav.ini      configuration
#   |- README.md  README.en.md  LICENSE  RELEASE_NOTES.md
#
# Install = unzip into ACZero\ (or copy those files in). Uninstall = delete
# ACZPropNav.dll and XINPUT1_4.dll. There is no installer script and no build/
# subfolder any more - that is the whole point of this layout.
#
# Safety model: the payload is built from an explicit ALLOWLIST (never by copying
# a directory), then the finished zip is re-read and its member list diffed
# against that allowlist. work/ (the maintainer's scratch dir with copies of the
# game executable, extracted game assets and third-party downloads) and build/
# object files cannot leak by construction, and the run aborts if anything
# unexpected appears anyway.
#
# Exit codes: 0 success, 1 precondition/verification failure, 2 usage error.
# =============================================================================
set -euo pipefail

PROG=${0##*/}
VERSION=""
OUT_DIR=""
KEEP=0

# Everything that ships, relative to the repo root. Flat: no subdirectory.
PAYLOAD_FILES=(
    ACZPropNav.dll
    XINPUT1_4.dll
    ACZPropNav.ini
    README.md
    README.en.md
    LICENSE
    RELEASE_NOTES.md
)
# The first two live in build/ in the checkout; the rest at the repo root.
FROM_BUILD=(ACZPropNav.dll XINPUT1_4.dll)

die() { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
section() { printf '\n== %s\n' "$*"; }

usage() {
    cat <<'EOF'
Assemble a clean, publishable ACZPropNav release zip.

Usage: tools/package_release.sh [options]

  --version X.Y.Z   release version (default: kVersion from src/core/mod.cpp)
  --out-dir DIR     where to write the zip (default: <repo>/dist)
  --keep            keep the staging tree (printed on exit) for inspection
  -h, --help        this help

The zip's contents extract straight into the game folder (ACZero\): the two
DLLs, ACZPropNav.ini, both READMEs, LICENSE and RELEASE_NOTES.md - flat, no
subdirectory, no installer. See docs/RELEASING.md.
EOF
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --version)      [ "$#" -ge 2 ] || die "--version needs a value"; VERSION=$2; shift 2 ;;
        --version=*)    VERSION=${1#*=}; shift ;;
        --out-dir)      [ "$#" -ge 2 ] || die "--out-dir needs a value"; OUT_DIR=$2; shift 2 ;;
        --out-dir=*)    OUT_DIR=${1#*=}; shift ;;
        --keep)         KEEP=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              printf '%s: unknown argument: %s\n\n' "$PROG" "$1" >&2; usage >&2; exit 2 ;;
    esac
done

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[ -d "$ROOT/src" ] || die "not a ACZPropNav checkout (no src/ under $ROOT)"
[ -d "$ROOT/build" ] || die "no build/ directory in $ROOT - build first: ./build.sh test"

command -v zip >/dev/null 2>&1 || die "zip(1) is required to create the release archive"

# ---- version ----------------------------------------------------------------
if [ -z "$VERSION" ]; then
    VERSION=$(sed -n 's/.*kVersion[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$ROOT/src/core/mod.cpp" 2>/dev/null | head -n1 || true)
    [ -n "$VERSION" ] || die "cannot read kVersion from src/core/mod.cpp; pass --version X.Y.Z"
fi
[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$ ]] \
    || die "bad version '$VERSION' (expected something like 1.0.0)"

if [ -z "$OUT_DIR" ]; then
    OUT_DIR=$ROOT/dist
else
    case $OUT_DIR in /*) ;; *) OUT_DIR=$PWD/$OUT_DIR ;; esac
fi
mkdir -p "$OUT_DIR"

# ---- 1. preconditions -------------------------------------------------------
section "checking inputs"
missing=0
for f in "${PAYLOAD_FILES[@]}"; do
    src=$ROOT/$f
    for b in "${FROM_BUILD[@]}"; do
        [ "$f" = "$b" ] && src=$ROOT/build/$f
    done
    if [ ! -f "$src" ]; then
        warn "missing: ${src#$ROOT/}"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || die "refusing to package an incomplete release (see above)"

# A stale/incomplete DLL is the classic packaging accident: check it is a real
# 64-bit PE and big enough to contain the guidance code.
for dll in "${FROM_BUILD[@]}"; do
    p=$ROOT/build/$dll
    size=$(wc -c <"$p")
    [ "$size" -gt 4096 ] || die "$dll is only $size bytes - truncated build?"
    head -c 2 "$p" | grep -q 'MZ' || die "$dll does not start with MZ - not a PE file"
    if command -v file >/dev/null 2>&1; then
        file -b "$p" | grep -qi 'PE32+.*x86-64' \
            || die "$dll is not a PE32+ x86-64 image (wrong toolchain?)"
    fi
    printf '  ok  build/%-18s %s bytes\n' "$dll" "$size"
done
printf '  ok  ACZPropNav.ini, READMEs, LICENSE, RELEASE_NOTES.md\n'

# The INI is UTF-8 **with BOM** and CRLF-tolerant; a BOM-less or double-BOM copy
# still parses, but ship the canonical one so the comments render in Notepad.
if head -c 3 "$ROOT/ACZPropNav.ini" | od -An -tx1 | grep -qi 'ef bb bf'; then
    printf '  ok  ACZPropNav.ini has the UTF-8 BOM\n'
else
    warn "ACZPropNav.ini has no UTF-8 BOM (the Chinese comments may look wrong in some editors)"
fi

# ---- 2. stage ---------------------------------------------------------------
section "staging"
STAGE=$(mktemp -d -t aczproprelease.XXXXXX)
trap '[ "$KEEP" -eq 1 ] || rm -rf "$STAGE"' EXIT
DEST=$STAGE/ACZPropNav
mkdir -p "$DEST"

copy_in() { # copy_in <repo-relative-name>
    local name=$1 src=$ROOT/$1
    for b in "${FROM_BUILD[@]}"; do
        [ "$name" = "$b" ] && src=$ROOT/build/$name
    done
    cp -f "$src" "$DEST/$name"
    printf '  +  %-20s %s bytes\n' "$name" "$(wc -c <"$DEST/$name")"
}

for f in "${PAYLOAD_FILES[@]}"; do copy_in "$f"; done

# Deterministic timestamps when the caller pins SOURCE_DATE_EPOCH.
if [ -n "${SOURCE_DATE_EPOCH:-}" ] && command -v touch >/dev/null 2>&1; then
    find "$DEST" -exec touch -d "@$SOURCE_DATE_EPOCH" {} + 2>/dev/null || true
fi

# The verification below reads the finished zip back with `unzip`. Check up front:
# a missing unzip used to surface as "line 176: unzip: command not found" *after*
# the archive had been created, which reads like a packaging bug rather than a
# missing tool (hit on the Windows CI runner, whose MSYS2 env has zip but not unzip).
command -v zip >/dev/null 2>&1 || die "zip is required (Debian/Ubuntu: apt install zip; MSYS2: pacman -S zip)"
command -v unzip >/dev/null 2>&1 || die "unzip is required (Debian/Ubuntu: apt install unzip; MSYS2: pacman -S unzip)"

# ---- 3. archive -------------------------------------------------------------
ZIP=$OUT_DIR/ACZPropNav-$VERSION-win64.zip
section "creating archive"
rm -f "$ZIP"
( cd "$STAGE" && zip -r -q -X "$ZIP" ACZPropNav )
printf '  %s\n' "$ZIP"

# ---- 4. verify the FINISHED archive -----------------------------------------
# Re-read the zip and diff its members against the allowlist: this is what makes
# a leak of work/ or build/obj/ impossible even if the staging logic is wrong.
section "verifying archive"
ACTUAL=$(unzip -Z1 "$ZIP" | sed 's#^ACZPropNav/##' | grep -v '/$' | grep -v '^$' | sort)
EXPECTED=$(printf '%s\n' "${PAYLOAD_FILES[@]}" | grep -v '^$' | sort)
if [ "$ACTUAL" != "$EXPECTED" ]; then
    printf 'unexpected members in the archive:\n' >&2
    diff <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$ACTUAL") >&2 || true
    die "archive content does not match the allowlist"
fi
for forbidden in '/work/' 'build/obj' '.pdb' '.lib' 'acz.exe' 'acz_copy'; do
    if unzip -Z1 "$ZIP" | grep -q -- "$forbidden"; then
        die "forbidden path '$forbidden' leaked into the archive"
    fi
done
# Every member must decompress (catches a truncated/corrupt zip).
unzip -t -qq "$ZIP" || die "archive failed its integrity test"
printf '  ok  %d files, allowlist matches, integrity ok\n' "$(printf '%s\n' "$ACTUAL" | wc -l | tr -d ' ')"

# ---- 5. report --------------------------------------------------------------
section "done"
printf 'archive : %s\n' "$ZIP"
printf 'size    : %s bytes\n' "$(wc -c <"$ZIP")"
if command -v sha256sum >/dev/null 2>&1; then
    printf 'sha256  : %s\n' "$(sha256sum "$ZIP" | cut -d' ' -f1)"
fi
printf 'members :\n'
printf '%s\n' "$ACTUAL" | sed 's/^/  /'
cat <<EOF

The user unzips this into the game folder, next to acz.exe:
    ...\\steamapps\\common\\ACE COMBAT ZERO THE BELKAN WAR (Steam Version)\\ACZero\\

Next: git tag -a v$VERSION -m "ACZPropNav $VERSION", push the tag, attach this
single zip to the GitHub Release, and paste the sha256 into the release notes.
Do NOT commit dist/ (gitignored). See docs/RELEASING.md.
EOF
if [ "$KEEP" -eq 1 ]; then printf '\nstaging tree kept at: %s\n' "$STAGE"; fi
