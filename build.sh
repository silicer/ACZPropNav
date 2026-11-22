#!/usr/bin/env bash
# ACZPropNav build script (MinGW-w64 / LLVM-MinGW toolchain).
#
#   ./build.sh           build everything into build/
#   ./build.sh test      build, then run every offline test
#   ./build.sh clean     remove build/
#
# This exists because GNU make's jobserver pipes do not always survive odd
# Windows/CI sandboxes; it performs exactly the same steps as the Makefile.
set -u

# Toolchain selection. Two naming conventions exist for the same compiler:
#   * Debian/Ubuntu mingw-w64 packages install prefixed binaries
#     (x86_64-w64-mingw32-g++), which is what the maintainer's Linux checkout uses;
#   * MSYS2's MINGW64 environment installs plain `gcc`/`g++` that already target
#     x86_64-w64-mingw32, so the prefixed name does not exist there (and CI on a
#     Windows runner runs inside that environment).
# Pick whichever is actually on PATH, unless the caller overrode CC/CXX.
if [ -z "${CXX:-}" ]; then
    if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        CXX=x86_64-w64-mingw32-g++
    elif command -v g++ >/dev/null 2>&1; then
        CXX=g++
    else
        CXX=x86_64-w64-mingw32-g++
    fi
fi
if [ -z "${CC:-}" ]; then
    case $CXX in
        *mingw32-g++) CC=${CXX%g++}gcc ;;
        *)            CC=${CXX%g++}gcc ;;
    esac
    command -v "$CC" >/dev/null 2>&1 || CC=$CXX
fi
BUILD=build
SRC=src

CXXFLAGS="-std=c++17 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter \
 -Isrc -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX"
LDFLAGS="-static -static-libgcc -static-libstdc++ -s"

# ./build.sh debug builds an UNSTRIPPED variant (-O1 -g) into build/debug/
# so a player-submitted crash dump can be turned into file:line. The shipping binaries
# stay stripped: that is what removes the MinGW runtime dependency footprint.
DEBUG_BUILD=0
if [ "${1:-}" = "debug" ]; then
    DEBUG_BUILD=1
    CXXFLAGS="${CXXFLAGS//-O2/-O1}"
    CXXFLAGS="$CXXFLAGS -g"
    LDFLAGS="-static -static-libgcc -static-libstdc++"
    BUILD=build/debug
    shift
fi
DEFS="-lkernel32 -luser32 -lpsapi -ladvapi32"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
step()  { printf '\n\033[36m== %s\033[0m\n' "$*"; }

mkdir -p "$BUILD/obj" "$BUILD/obj/tools" || exit 1

compile() { # compile <cpp> -> echoes object path
    local src="$1"
    local obj
    obj="$BUILD/obj/${src#$SRC/}"
    obj="${obj%.cpp}.o"
    mkdir -p "$(dirname "$obj")" || return 1
    # No incremental build: the project is small and a stale object after a
    # header change would silently mix ABIs. Always recompile.
    if ! $CXX $CXXFLAGS -c "$src" -o "$obj"; then
        red "compile failed: $src"; return 1
    fi
    echo "$obj"
}

compile_all() { # compile every src/**/*.cpp, print objects
    local objs=""
    while IFS= read -r src; do
        local obj
        obj=$(compile "$src") || return 1
        objs="$objs $obj"
    done < <(find "$SRC" -name '*.cpp' | sort)
    echo "$objs"
}

link_shared() { # link_shared <out> <objs...>
    local out="$1"; shift
    $CXX $LDFLAGS -shared -o "$out" "$@" 2>&1 || { red "link failed: $out"; return 1; }
    green "built $out"
}

link_exe() { # link_exe <out> <objs...>
    local out="$1"; shift
    $CXX $LDFLAGS -o "$out" "$@" $DEFS 2>&1 || { red "link failed: $out"; return 1; }
    green "built $out"
}

do_all() {
    step "compiling $SRC"
    local objs
    objs=$(compile_all) || { red "compile step failed"; return 1; }

    step "mod DLL"
    if [ -f loader/exports.def ]; then
        link_shared "$BUILD/ACZPropNav.dll" $objs loader/exports.def || return 1
    else
        red "loader/exports.def missing (owned by loader-hook) - skipping mod DLL"
    fi

    step "proxy loader"
    if [ -f loader/xinput1_4_proxy.cpp ] && [ -f loader/xinput1_4.def ]; then
        $CXX $CXXFLAGS -c loader/xinput1_4_proxy.cpp -o "$BUILD/obj/loader_xinput.o" 2>&1 || return 1
        link_shared "$BUILD/XINPUT1_4.dll" "$BUILD/obj/loader_xinput.o" loader/xinput1_4.def || return 1
    else
        red "loader proxy sources missing - skipping XINPUT1_4.dll"
    fi
    if [ -f loader/version_proxy.cpp ] && [ -f loader/version.def ]; then
        $CXX $CXXFLAGS -c loader/version_proxy.cpp -o "$BUILD/obj/loader_version.o" 2>&1 || return 1
        link_shared "$BUILD/VERSION.dll" "$BUILD/obj/loader_version.o" loader/version.def || return 1
    fi

    # Ship-ready copy of the config next to the DLL. Always re-copied from the repo
    # root so build/ can never carry a stale INI (it did: a 10-day-old copy with
    # different documented defaults shipped next to the DLL and confused testers).
    step "config"
    if [ -f ACZPropNav.ini ]; then
        cp -f ACZPropNav.ini "$BUILD/ACZPropNav.ini" && green "copied ACZPropNav.ini"
    else
        red "ACZPropNav.ini missing at the repo root"
    fi

    step "offline tools"
    # Each tool links only what it needs; missing sources are reported, not fatal.
    build_tool() { # build_tool <exe> <cpp list...>
        local out="$1"; shift
        local objs=""
        for src in "$@"; do
            [ -f "$src" ] || { red "missing $src"; return 0; }
            local o
            if [[ "$src" == src/* ]]; then
                o=$(compile "$src") || return 0
            else
                o="$BUILD/obj/tools/$(basename "${src%.cpp}").o"
                mkdir -p "$(dirname "$o")"
                $CXX $CXXFLAGS -c "$src" -o "$o" 2>&1 || { red "compile failed: $src"; return 0; }
            fi
            objs="$objs $o"
        done
        link_exe "$out" $objs
    }

    build_tool "$BUILD/initest.exe"  tools/initest/main.cpp src/core/config.cpp src/core/log.cpp
    build_tool "$BUILD/pnsim.exe"    tools/pnsim/main.cpp   src/pn/pn.cpp
    build_tool "$BUILD/memtest.exe"  tools/memtest/main.cpp src/mem/mem.cpp src/mem/discovery.cpp src/core/log.cpp
    build_tool "$BUILD/hooktest.exe" tools/hooktest/main.cpp src/hook/hook.cpp src/core/log.cpp
    # Run B offline proof: hardware data breakpoints + the one-shot VEH continuation scheme.
    build_tool "$BUILD/hwwatchtest.exe" tools/hwwatchtest/main.cpp
    build_tool "$BUILD/smoke.exe"    tools/smoke/main.cpp
}

# Can this host execute the Windows PE test binaries we just built?
can_run_pe() {
    [ -x "$BUILD/pnsim.exe" ] || return 1
    "$BUILD/pnsim.exe" --help >/dev/null 2>&1 && return 0
    return 1
}

# The guidance math (pnsim) and the INI parser (initest) are hosted on the native
# compiler where possible: on a Linux cross-build host there is no Wine, so the PE
# tests cannot run at all - and silently reporting "SOME TESTS FAILED" for an
# Exec-format-error would train people to ignore the suite. pnsim has no Win32
# dependency beyond that, so the math layer is verified natively here.
do_native_tests() {
    local hostcxx=${HOSTCXX:-g++}
    command -v "$hostcxx" >/dev/null 2>&1 || { red "skip native tests (no $hostcxx)"; return 0; }
    local rc=0
    step "test: pnsim --selftest (native, guidance math)"
    "$hostcxx" -std=c++17 -O2 -Isrc -o "$BUILD/pnsim_native" tools/pnsim/main.cpp src/pn/pn.cpp \
        2>&1 | tail -5 || { red "native pnsim build failed"; return 1; }
    "$BUILD/pnsim_native" --selftest || rc=1
    return $rc
}

do_test() {
    local rc=0
    if can_run_pe; then
        for t in initest pnsim memtest hooktest hwwatchtest; do
            [ -x "$BUILD/$t.exe" ] || { red "skip $t (not built)"; continue; }
            step "test: $t"
            if [ "$t" = "pnsim" ]; then
                "$BUILD/$t.exe" --selftest || rc=1
            else
                "$BUILD/$t.exe" || rc=1
            fi
        done
        # End-to-end: load the real DLL in a synthetic process and check its log.
        if [ -x "$BUILD/smoke.exe" ] && [ -f "$BUILD/ACZPropNav.dll" ]; then
            step "test: smoke (DLL in-process)"
            mkdir -p "$BUILD/smoke_iso"
            cp -f "$BUILD/ACZPropNav.dll" "$BUILD/smoke_iso/" 2>/dev/null
            cp -f ACZPropNav.ini "$BUILD/smoke_iso/" 2>/dev/null
            cp -f "$BUILD/smoke.exe" "$BUILD/smoke_iso/" 2>/dev/null
            "$BUILD/smoke.exe" "$(cd "$BUILD/smoke_iso" && pwd)" || rc=1
        fi
    else
        step "host cannot execute Windows PE binaries"
        red "PE tests (initest/memtest/hooktest/hwwatchtest/smoke) NOT RUN on this host."
        red "Run ./build.sh test on Windows (or install wine) for the full suite."
        do_native_tests || rc=1
    fi
    if [ $rc -eq 0 ]; then
        green "ALL RUNNABLE TESTS PASSED"
    else
        red "SOME TESTS FAILED"
        [ -n "${CI:-}" ] && exit 1
    fi
    return $rc
}

case "${1:-all}" in
    clean) rm -rf "$BUILD"; green "cleaned";;
    test)  do_all && do_test;;
    all|"") do_all;;
    *) red "usage: ./build.sh [all|test|clean|debug]"; exit 2;;
esac
