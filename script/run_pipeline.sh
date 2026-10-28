#!/usr/bin/env bash
# run_pipeline.sh — Grace obliging GR(1) pipeline runner (BDD mode)
#
# Usage:
#   ./script/run_pipeline.sh benchmarks/<name>/arena.txt [--mode transform|synth|play|all] [options]
#
# Modes:
#   transform write obliging arena + full spec only
#   synth run fairsyn for all subgames (BDD mode, coverage pruning)
#   play run Algorithm 3 (requires prior synth run)
#   all transform + synth + play  (default)
#
#   --mode         <transform|synth|play|all>   (default: all)
#   --steps        <N>     play steps (default: 500)
#   --k            <N>     resample every k Eve steps (default: 1 (Algo 3))
#   --alpha        <f>     initial decay rate all monitors (default: 0.5)
#   --alpha-prime  <f>     override alpha for union monitor only  (default: = alpha)
#   --lambda       <f>     attenuation rate, all monitors    (default: 0.9)
#   --lambda-prime <f>     override lambda for union monitor only   (default: = lambda)
#   --eps          <f>     monitor-output clipping epsilon (default: binary's, 1e-9)
#   --seed         <N>     RNG seed (default: 42)
#   --trace        <file>  Adam strategy file
#   --ainf         <set>   true A_inf for convergence report
#   --parallel     parallel synthesis
#   --skip-delete  keep temp dirs after synthesis (for debugging)
#   --outdir       <dir>   override output dir (default: out/ next to arena.txt)
#   --fairsyn      <path>  controller_synthesis binary
#   --ctrl-query   <path>  controller_query binary
#   --owl          <path>  owl bin directory
#   --ubdd-type    <N>     BDD backend: 0 CUDD, 1 Sylvan, 2 Sylvan+parallel (default: 2)
#   --n-workers    <N>     fairsyn worker threads per call (default: auto)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Tool paths (override via environment)
FAIRSYN="${GRACE_FAIRSYN:-$ROOT/External_tools/fairsyn/build/bin/controller_synthesis}"
CTRL_QUERY="${GRACE_CTRL_QUERY:-$ROOT/External_tools/fairsyn/build/bin/controller_query}"
OWL_PATH="${GRACE_OWL:-$ROOT/External_tools/owl/owl-linux-musl-amd64-21.0/bin}"

# Locate compiled binary (grace or legacy main)
if   [[ -x "$SCRIPT_DIR/grace" ]];    then BIN="$SCRIPT_DIR/grace"
elif [[ -x "$SCRIPT_DIR/main" ]];     then BIN="$SCRIPT_DIR/main"
elif [[ -x "$SCRIPT_DIR/../grace" ]]; then BIN="$(cd "$SCRIPT_DIR/.." && pwd)/grace"
elif [[ -x "$SCRIPT_DIR/../main" ]];  then BIN="$(cd "$SCRIPT_DIR/.." && pwd)/main"
else
    echo "[error] Cannot find compiled binary (grace or main). Run 'make' first." >&2
    exit 1
fi
 
# Defaults
MODE="all"
STEPS=500
K=1
ALPHA=0.9
ALPHA_PRIME=0.5
LAMBDA=0.9
LAMBDA_PRIME=0.9
SEED=42
DEBUG_STATES=0
TRACE=""
PARALLEL=""
SKIP_DELETE=""
NO_PRUNE=""
PLOT=0
OUTDIR=""
GAME_FILE=""
UBDD_TYPE=0
 
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)         MODE="$2";             shift 2 ;;
        --fairsyn)      FAIRSYN="$2";          shift 2 ;;
        --ctrl-query)   CTRL_QUERY="$2";       shift 2 ;;
        --owl)          OWL_PATH="$2";         shift 2 ;;
        --steps)        STEPS="$2";            shift 2 ;;
        --k)            K="$2";                shift 2 ;;
        --alpha)        ALPHA="$2";            shift 2 ;;
        --alpha-prime)  ALPHA_PRIME="$2";      shift 2 ;;
        --lambda)       LAMBDA="$2";           shift 2 ;;
        --lambda-prime) LAMBDA_PRIME="$2";     shift 2 ;;
        --seed)         SEED="$2";             shift 2 ;;
        --debug-states) DEBUG_STATES="$2";     shift 2 ;;
        --trace)        TRACE="$2";            shift 2 ;;
        --parallel)     PARALLEL="--parallel"; shift   ;;
        --skip-delete)  SKIP_DELETE="--skip-delete"; shift ;;
        --no-prune)     NO_PRUNE="--no-prune"; shift ;;
        --plot)         PLOT=1;                shift   ;;
        --outdir)       OUTDIR="$2";           shift 2 ;;
        --ubdd-type)    UBDD_TYPE="$2";        shift 2 ;;
        --help|-h)
            sed -n '2,40p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        -*) echo "[error] Unknown option: $1" >&2; exit 1 ;;
        *)
            if [[ -z "$GAME_FILE" ]]; then GAME_FILE="$1"
            else echo "[error] Unexpected argument: $1" >&2; exit 1; fi
            shift ;;
    esac
done
 
[[ -z "$GAME_FILE" ]] && { echo "[error] No game file specified." >&2; exit 1; }
[[ ! -f "$GAME_FILE" ]] && { echo "[error] File not found: $GAME_FILE" >&2; exit 1; }
 
case "$MODE" in
    transform|synth|play|all) ;;
    *) echo "[error] Unknown mode '$MODE'" >&2; exit 1 ;;
esac
 
# Validate required binaries early so we fail clearly before doing any work
if [[ "$MODE" == "synth" || "$MODE" == "all" ]]; then
    [[ ! -x "$FAIRSYN" ]] && {
        echo "[error] controller_synthesis not found or not executable: $FAIRSYN" >&2
        echo "        Edit FAIRSYN= at the top of this script or pass --fairsyn" >&2
        exit 1
    }
    [[ ! -x "$CTRL_QUERY" ]] && {
        echo "[error] controller_query not found or not executable: $CTRL_QUERY" >&2
        echo "        Edit CTRL_QUERY= at the top of this script or pass --ctrl-query" >&2
        exit 1
    }
fi
if [[ "$MODE" == "play" ]]; then
    [[ ! -x "$CTRL_QUERY" ]] && {
        echo "[error] controller_query not found or not executable: $CTRL_QUERY" >&2
        exit 1
    }
fi
 
STEM="$(basename "$GAME_FILE" .txt)"
GAME_DIR="$(cd "$(dirname "$GAME_FILE")" && pwd)"
OUTDIR="${OUTDIR:-$GAME_DIR/$STEM}"
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
 
CSV="$OUTDIR/play.csv"
PLAY_LOG="$OUTDIR/play.log"
 
echo "========================================================"
echo " Grace — Obliging GR(1) Synthesis Pipeline"
echo "========================================================"
echo " Game        : $GAME_FILE"
echo " Mode        : $MODE"
echo " Output dir  : $OUTDIR"
echo " Binary      : $BIN"
[[ -n "$PARALLEL" ]] && echo " Parallel    : yes"
echo "========================================================"
echo ""
 
# ── Transform ─────────────────────────────────────────────────────────────────
run_transform() {
    echo "[transform] Writing obliging arena and full spec..."
    "$BIN" "$GAME_FILE" transform --output "$OUTDIR"
    echo ""
}
 
# ── Synthesis ──────────────────────────────────────────────────────────────────
run_synth() {
    local SYNTH_LOG="$OUTDIR/synth.log"
    echo "[synth] Running synthesis loop (BDD mode)..."
    echo "[synth] Output logged to: $SYNTH_LOG"
    "$BIN" "$GAME_FILE" synth \
        --fairsyn          "$FAIRSYN" \
        --controller-query "$CTRL_QUERY" \
        --owl-path         "$OWL_PATH" \
        --workdir          "$OUTDIR" \
        $PARALLEL \
        --ubdd-type        "$UBDD_TYPE" \
        $SKIP_DELETE \
        $NO_PRUNE \
        2>&1 | tee "$SYNTH_LOG"
    echo ""
}
 
# ── Play ───────────────────────────────────────────────────────────────────────
run_play() {
    # Check that at least one subgame BDD exists
    local found=0
    for d in "$OUTDIR"/A*/; do
        [[ -f "$d/controller.bdd" ]] && { found=1; break; }
    done
    if [[ $found -eq 0 ]]; then
        echo "[error] No synthesized strategies (controller.bdd) found in $OUTDIR" >&2
        echo "        Run synthesis first:" >&2
        echo "          $0 $GAME_FILE --mode synth" >&2
        exit 1
    fi
 
    echo "[play] Running Algorithm 3 for $STEPS steps..."
    PLAY_ARGS=(
        "$GAME_FILE" play
        --workdir          "$OUTDIR"
        --controller-query "$CTRL_QUERY"
        --steps            "$STEPS"
        --k                "$K"
        --alpha            "$ALPHA"
        --alpha-prime      "$ALPHA_PRIME"
        --lambda           "$LAMBDA"
        --lambda-prime     "$LAMBDA_PRIME"
        --seed             "$SEED"
        --debug-states     "$DEBUG_STATES"
        --csv              "$CSV"
    )
    [[ -n "$TRACE" ]] && PLAY_ARGS+=(--trace "$TRACE")
 
    "$BIN" "${PLAY_ARGS[@]}" | tee "$PLAY_LOG"
 
    if [[ $PLOT -eq 1 && -f "$CSV" ]]; then
        local PLOT_OUT="$OUTDIR/play_plot.png"
        echo "[plot] Generating $PLOT_OUT ..."
        python3 "$SCRIPT_DIR/plot_policy.py" "$CSV" --out "$PLOT_OUT" \
            && echo "[plot] Done." \
            || echo "[plot] Warning: plot failed (check plot_policy.py)" >&2
    fi
 
    echo ""
    echo "========================================================"
    echo " Play log : $PLAY_LOG"
    echo " CSV      : $CSV"
    [[ $PLOT -eq 1 ]] && echo " Plot     : $OUTDIR/play_plot.png"
    echo "========================================================"
}
 
# ── Dispatch ───────────────────────────────────────────────────────────────────
case "$MODE" in
    transform) run_transform ;;
    synth)     run_synth     ;;
    play)      run_play      ;;
    all)       run_transform; run_synth; run_play ;;
esac