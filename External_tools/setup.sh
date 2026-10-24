#!/usr/bin/env bash
# setup.sh — fetch and build the external tools Grace depends on
#
# usage: ./setup.sh <tool> [<tool> ...]
#   genie    clone Genie, build its dependencies (CUDD, Sylvan, cpphoafparser) and libgenie.a
#   fairsyn  everything genie does, then clone and build FairSyn (does not rebuild if genie is already built; ASSUMES the existing build is compatible and correctly done)
#
# Everything is cloned, downloaded and built inside External_tools/, nothing goes to /usr/local:
#   genie/                     Genie sources; genie/build/libgenie.a is the compiled library
#   genie_deps/src/<lib>       dependency sources
#   genie_deps/install         install prefix for the dependencies (pass to FairSyn as CMAKE_PREFIX_PATH)
#   fairsyn/                   FairSyn sources; binaries land in fairsyn/build/bin
#
# Every repository is pinned to a fixed commit (override with GENIE_REV, FAIRSYN_REV, CUDD_REV)
# Re-running is cheap: finished steps are skipped. Set CLEAN=1 to rebuild from scratch

set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

step() { echo; echo "==> $*"; }
die() { echo "[error] $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1${2:+ ($2)}"; }

usage() {
    sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

download() {
    if command -v curl >/dev/null 2>&1; then curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then wget -q -O "$2" "$1"
    else die "need curl or wget to download $1"; fi
}

# fetch_repo <url> <dir> <rev>: clone <url> into <dir> if missing and check out <rev>
fetch_repo() {
    local url="$1" dir="$2" rev="$3"
    [[ -d "$dir/.git" ]] || git clone "$url" "$dir"
    if [[ "$(git -C "$dir" rev-parse HEAD)" != "$(git -C "$dir" rev-parse -q --verify "$rev^{commit}" || true)" ]]; then
        git -C "$dir" fetch --tags origin
        git -C "$dir" checkout -q --detach "$rev" || die "could not check out $rev in $dir (local changes?)"
    fi
    echo "  revision: $(git -C "$dir" rev-parse HEAD)"
}

# ── Genie ────────────────────────────────────────────────────────────────────
GENIE_DIR="$TOOLS_DIR/genie"
GENIE_GIT="https://gitlab.com/kmallik/genie.git"
GENIE_REV="${GENIE_REV:-bc570d2cbfcbe77bcc5e1a4f8e2f72df14c51e04}"
# the prefix must live outside genie/: CMake refuses exported include paths inside the source tree
GENIE_SUB="$TOOLS_DIR/genie_deps/src"
GENIE_DEPS="$TOOLS_DIR/genie_deps/install"

CUDD_GIT="https://github.com/ivmai/cudd"
CUDD_REV="${CUDD_REV:-f54f533303640afd5dbe47a05ebeabb3066f2a25}"
SYLVAN_GIT="https://github.com/trolando/sylvan"
SYLVAN_TAG="v1.7.0"
HOAF_URL="https://automata.tools/hoa/cpphoafparser/down/cpphoafparser-0.99.2.tgz"

GENIE_DONE=0

setup_genie() {
    [[ "$GENIE_DONE" == "1" ]] && return

    step "Genie: checking prerequisites"
    for t in git make cmake c++ autoreconf automake pkg-config tar; do need "$t"; done
    # CUDD's autoreconf needs libtoolize; Homebrew installs it as glibtoolize
    if ! command -v libtoolize >/dev/null 2>&1; then
        command -v glibtoolize >/dev/null 2>&1 || die "libtool not found (macOS: brew install libtool, Linux: apt install libtool)"
        export LIBTOOLIZE=glibtoolize
    fi
    # Homebrew prefix holds GMP (needed by Sylvan) on macOS
    if command -v brew >/dev/null 2>&1; then
        local brew_prefix
        brew_prefix="$(brew --prefix)"
        export CMAKE_PREFIX_PATH="$brew_prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
    fi

    if [[ "${CLEAN:-0}" == "1" ]]; then
        step "Genie: CLEAN=1, removing previous builds"
        rm -rf "$TOOLS_DIR/genie_deps" "$GENIE_DIR/build"
    fi
    mkdir -p "$GENIE_SUB" "$GENIE_DEPS/include" "$GENIE_DEPS/lib"

    step "Genie: sources"
    fetch_repo "$GENIE_GIT" "$GENIE_DIR" "$GENIE_REV"

    step "Genie: CUDD"
    if [[ -f "$GENIE_DEPS/lib/libcudd.a" && -f "$GENIE_DEPS/include/cudd.h" ]]; then
        echo "  already installed"
    else
        fetch_repo "$CUDD_GIT" "$GENIE_SUB/cudd" "$CUDD_REV"
        (
            cd "$GENIE_SUB/cudd"
            autoreconf -f -i
            ./configure --enable-shared --enable-obj --enable-dddmp --prefix="$GENIE_DEPS"
            make -j "$JOBS"
            [[ "${RUN_CHECKS:-0}" == "1" ]] && make check
            make install
            # Genie includes these internal CUDD headers directly
            cp config.h util/util.h "$GENIE_DEPS/include/"
        )
    fi
    # util.h's fail(why) macro clobbers std::ios::fail() in libc++ <complex>; C++ users never need it
    if ! grep -q "setup.sh: undef fail" "$GENIE_DEPS/include/util.h"; then
        printf '\n/* setup.sh: undef fail for C++ */\n#ifdef __cplusplus\n#undef fail\n#endif\n' >> "$GENIE_DEPS/include/util.h"
    fi

    step "Genie: Sylvan $SYLVAN_TAG"
    if [[ -f "$GENIE_DEPS/lib/libsylvan.a" && -f "$GENIE_DEPS/include/sylvan.h" ]]; then
        echo "  already installed"
    else
        [[ -d "$GENIE_SUB/sylvan/.git" ]] || git clone --branch "$SYLVAN_TAG" --depth 1 "$SYLVAN_GIT" "$GENIE_SUB/sylvan"
        # v1.7.0 builds with -Werror, which newer compilers trip on (e.g. -Wunused-but-set-parameter)
        sed -i.orig 's/ -Werror//' "$GENIE_SUB/sylvan/src/CMakeLists.txt"
        # CMAKE_POLICY_VERSION_MINIMUM lets CMake >= 4 configure Sylvan's old cmake_minimum_required
        cmake -S "$GENIE_SUB/sylvan" -B "$GENIE_SUB/sylvan/build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$GENIE_DEPS" \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            -DBUILD_SHARED_LIBS=OFF
        cmake --build "$GENIE_SUB/sylvan/build" -j "$JOBS"
        cmake --install "$GENIE_SUB/sylvan/build"
    fi

    step "Genie: cpphoafparser"
    if [[ -f "$GENIE_DEPS/include/cpphoafparser/parser/hoa_parser.hh" ]]; then
        echo "  already installed"
    else
        # header-only: unpack and copy the headers into the prefix
        local tgz="$GENIE_SUB/cpphoafparser.tgz"
        [[ -f "$tgz" ]] || download "$HOAF_URL" "$tgz"
        rm -rf "$GENIE_SUB/cpphoafparser"
        mkdir -p "$GENIE_SUB/cpphoafparser"
        tar xzf "$tgz" -C "$GENIE_SUB/cpphoafparser" --strip-components 1
        cp -R "$GENIE_SUB/cpphoafparser/include/." "$GENIE_DEPS/include/"
    fi

    step "Genie: building libgenie"
    cmake -S "$GENIE_DIR" -B "$GENIE_DIR/build" \
        -DCMAKE_BUILD_TYPE=release \
        -DCMAKE_PREFIX_PATH="$GENIE_DEPS${CMAKE_PREFIX_PATH:+;${CMAKE_PREFIX_PATH//:/;}}"
    cmake --build "$GENIE_DIR/build" -j "$JOBS"
    [[ -f "$GENIE_DIR/build/libgenie.a" ]] || die "expected $GENIE_DIR/build/libgenie.a after build"

    step "Genie ready"
    echo "  library      : $GENIE_DIR/build/libgenie.a"
    echo "  dependencies : $GENIE_DEPS"
    GENIE_DONE=1
}

# FairSyn
FAIRSYN_DIR="$TOOLS_DIR/fairsyn"
FAIRSYN_GIT="https://gitlab.com/kmallik/fairsyn.git"
FAIRSYN_REV="${FAIRSYN_REV:-2791495f3d5f9bda6a42f302d11cdd7286ff68aa}"

setup_fairsyn() {
    setup_genie

    step "FairSyn: sources"
    fetch_repo "$FAIRSYN_GIT" "$FAIRSYN_DIR" "$FAIRSYN_REV"
    [[ "${CLEAN:-0}" == "1" ]] && rm -rf "$FAIRSYN_DIR/build"

    step "FairSyn: building"
    cmake -S "$FAIRSYN_DIR" -B "$FAIRSYN_DIR/build" \
        -DCMAKE_BUILD_TYPE=release \
        -Dgenie_HOME="$GENIE_DIR" \
        -DCMAKE_PREFIX_PATH="$GENIE_DEPS${CMAKE_PREFIX_PATH:+;${CMAKE_PREFIX_PATH//:/;}}"
    cmake --build "$FAIRSYN_DIR/build" -j "$JOBS"
    for b in controller_synthesis controller_query; do
        [[ -x "$FAIRSYN_DIR/build/bin/$b" ]] || die "expected $FAIRSYN_DIR/build/bin/$b after build"
    done

    step "FairSyn ready"
    echo "  controller_synthesis : $FAIRSYN_DIR/build/bin/controller_synthesis"
    echo "  controller_query     : $FAIRSYN_DIR/build/bin/controller_query"
}

# ── Dispatch ─────────────────────────────────────────────────────────────────
[[ $# -gt 0 ]] || usage 1

for tool in "$@"; do
    case "$tool" in
        genie) setup_genie ;;
        fairsyn) setup_fairsyn ;;
        -h|--help) usage 0 ;;
        *) echo "[error] unknown tool '$tool'" >&2; usage 1 ;;
    esac
done
