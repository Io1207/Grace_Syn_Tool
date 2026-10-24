#!/usr/bin/env bash
# build_all.sh — Build everything needed to run Grace from a fresh checkout:
#   1. Owl 21.0 (linux-amd64)
#   2. FairSyn, with Genie and Genie's libraries (CUDD, Sylvan, cpphoafparser)
#   3. Grace
#
# Local copies in third_party/ are always preferred, so a pre-fetched (archived) artifact builds fully offline

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # artifact root (this script lives in build/)
TP="$ROOT/third_party"
JOBS="${JOBS:-$(nproc)}"

FAIRSYN_GIT="${FAIRSYN_GIT:-https://gitlab.com/kmallik/fairsyn.git}"
FAIRSYN_SRC="${FAIRSYN_SRC:-$TP/fairsyn}"

GENIE_GIT="${GENIE_GIT:-https://gitlab.com/kmallik/genie.git}"
GENIE_SRC="${GENIE_SRC:-$TP/genie}"
GENIE_DEPS="${GENIE_DEPS:-$TP/genie-deps}"   # install prefix for CUDD, Sylvan, cpphoafparser

OWL_URL="${OWL_URL:-https://github.com/owl-toolkit/owl/releases/download/release-21.0/owl-linux-amd64-21.0.zip}"
OWL_ZIP="${OWL_ZIP:-$TP/owl-linux-amd64-21.0.zip}"
OWL_DIR="$TP/owl"

step() { echo; echo "==> $*"; }
warn() { echo "  [warn] $*" >&2; }
die() { echo "[error] $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"; }

download() {
    if command -v curl >/dev/null 2>&1; then curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then wget -q -O "$2" "$1"
    else die "need curl or wget to download $1"; fi
}

mkdir -p "$TP"

# ── Owl ──────────────────────────────────────────────────────────────────────
step "Owl 21.0"
if [[ -x "$OWL_DIR/bin/owl" ]]; then
    echo "  already present: $OWL_DIR"
else
    need unzip
    if [[ ! -f "$OWL_ZIP" ]]; then
        echo "downloading $OWL_URL"
        download "$OWL_URL" "$OWL_ZIP.part"
        mv "$OWL_ZIP.part" "$OWL_ZIP"
    fi
    tmp="$(mktemp -d)"
    unzip -q "$OWL_ZIP" -d "$tmp"
    top="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -n1)"
    rm -rf "$OWL_DIR"
    mv "${top:-$tmp}" "$OWL_DIR"
    rm -rf "$tmp"
    chmod +x "$OWL_DIR/bin/"* 2>/dev/null || true
fi
[[ -x "$OWL_DIR/bin/owl" ]] || die "owl binary not found at $OWL_DIR/bin/owl"

# ── FairSyn sources ──────────────────────────────────────────────────────────
step "FairSyn sources"
if [[ ! -f "$FAIRSYN_SRC/CMakeLists.txt" ]]; then
    need git
    echo "  cloning $FAIRSYN_GIT"
    git clone --recursive "$FAIRSYN_GIT" "$FAIRSYN_SRC"
fi
if [[ -d "$FAIRSYN_SRC/.git" ]]; then
    echo "  revision: $(git -C "$FAIRSYN_SRC" rev-parse HEAD)"
else
    echo "  using vendored sources (no .git): $FAIRSYN_SRC"
fi

# Genie sources
step "Genie sources"
if [[ ! -f "$GENIE_SRC/CMakeLists.txt" ]]; then
    need git
    echo "  cloning $GENIE_GIT"
    git clone "$GENIE_GIT" "$GENIE_SRC"
fi
if [[ -d "$GENIE_SRC/.git" ]]; then
    echo "  revision: $(git -C "$GENIE_SRC" rev-parse HEAD)"
else
    echo "  using vendored sources (no .git): $GENIE_SRC"
fi

if [[ "${FETCH_ONLY:-0}" == "1" ]]; then
    step "Dependencies fetched (FETCH_ONLY=1), skipping builds"
    exit 0
fi

step "Checking build prerequisites"
need cmake
need c++
echo "  cmake   : $(cmake --version | head -n1)"
echo "  c++     : $(c++ --version | head -n1)"
if ! python3 -c "import pandas, numpy, matplotlib" >/dev/null 2>&1; then
    warn "Python plotting packages missing (only needed for plots): pip install -r build/requirements.txt"
fi

step "Genie dependencies (CUDD, Sylvan, cpphoafparser)"
if [[ -f "$GENIE_DEPS/.installed" ]]; then
    echo "  already present: $GENIE_DEPS"
else
    for t in git make pkg-config autoreconf libtoolize wget tar; do need "$t"; done
    mkdir -p "$GENIE_DEPS"
    # install.sh clones/builds into ./submodules, so run it from the Genie checkout
    (cd "$GENIE_SRC" && bash ./install.sh "$GENIE_DEPS/")
    touch "$GENIE_DEPS/.installed"
fi

step "Building FairSyn"
cmake -S "$FAIRSYN_SRC" -B "$FAIRSYN_SRC/build" -DCMAKE_BUILD_TYPE=Release \
    -Dgenie_HOME="$GENIE_SRC" -DCMAKE_PREFIX_PATH="$GENIE_DEPS"
cmake --build "$FAIRSYN_SRC/build" -j "$JOBS"
for b in controller_synthesis controller_query; do
    [[ -x "$FAIRSYN_SRC/build/bin/$b" ]] || die "expected $FAIRSYN_SRC/build/bin/$b after build"
done

step "Building Grace"
cmake -S "$ROOT/build" -B "$ROOT/build/cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build/cmake" -j "$JOBS"
[[ -x "$ROOT/build/bin/grace" ]] || die "build/bin/grace not produced"

step "Build complete"
echo "  grace                : $ROOT/build/bin/grace"
echo "  controller_synthesis : $FAIRSYN_SRC/build/bin/controller_synthesis"
echo "  controller_query     : $FAIRSYN_SRC/build/bin/controller_query"
echo "  owl                  : $OWL_DIR/bin/owl"
echo
echo "Next: ./script/smoke_test.sh"