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

MODE="all"
STEPS=500
K=1
ALPHA=0.5
ALPHA_PRIME=""
LAMBDA=0.9
LAMBDA_PRIME=""
EPS=""
SEED=42
TRACE=""
AINF=""
PARALLEL=""
SKIP_DELETE=""
PLOT=0
OUTDIR=""
GAME_FILE=""
UBDD_TYPE=2
N_WORKERS=""

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
        --eps)          EPS="$2";              shift 2 ;;
        --seed)         SEED="$2";             shift 2 ;;
        --trace)        TRACE="$2";            shift 2 ;;
        --ainf)         AINF="$2";             shift 2 ;;
        --parallel)     PARALLEL="--parallel"; shift   ;;
        --skip-delete)  SKIP_DELETE="--skip-delete"; shift ;;
        --plot)         PLOT=1;                shift   ;;
        --outdir)       OUTDIR="$2";           shift 2 ;;
        --ubdd-type)    UBDD_TYPE="$2";        shift 2 ;;
        --n-workers)    N_WORKERS="$2";        shift 2 ;;
        --help|-h)
            sed -n '2,52p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        -*) echo "[error] Unknown option: $1" >&2; exit 1 ;;
        *)
            if [[ -z "$GAME_FILE" ]]; then GAME_FILE="$1"
            else echo "[error] Unexpected argument: $1" >&2; exit 1; fi
            shift ;;
    esac
done

if   [[ -x "$ROOT/build/bin/grace" ]]; then BIN="$ROOT/build/bin/grace"
elif [[ -x "$ROOT/src/main" ]]; then BIN="$ROOT/src/main"   # legacy Makefile build
else
    echo "[error] Cannot find grace binary. Run ./build/build_all.sh first or build using makeile in src" >&2
    exit 1
fi

[[ -z "$GAME_FILE" ]] && { echo "[error] No game file specified." >&2; exit 1; }
[[ ! -f "$GAME_FILE" ]] && { echo "[error] File not found: $GAME_FILE" >&2; exit 1; }

case "$MODE" in
    transform|synth|play|all) ;;
    *) echo "[error] Unknown mode '$MODE'" >&2; exit 1 ;;
esac

if [[ "$MODE" == "synth" || "$MODE" == "all" ]]; then
    if [[ ! -x "$FAIRSYN" ]]; then
        echo "[error] controller_synthesis not found or not executable: $FAIRSYN" >&2
        echo "        Run ./External_tools/setup.sh, set GRACE_FAIRSYN, or pass --fairsyn" >&2
        exit 1
    fi
fi
if [[ "$MODE" == "synth" || "$MODE" == "play" || "$MODE" == "all" ]]; then
    if [[ ! -x "$CTRL_QUERY" ]]; then
        echo "[error] controller_query not found or not executable: $CTRL_QUERY" >&2
        echo "        Run ./External_tools/setup.sh, set GRACE_CTRL_QUERY, or pass --ctrl-query" >&2
        exit 1
    fi
fi

GAME_DIR="$(cd "$(dirname "$GAME_FILE")" && pwd)"
if [[ "$(basename "$GAME_FILE")" == "arena.txt" ]]; then
    DEFAULT_OUT="$GAME_DIR/out"
else
    DEFAULT_OUT="$GAME_DIR/$(basename "$GAME_FILE" .txt)"
fi
OUTDIR="${OUTDIR:-$DEFAULT_OUT}"
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"

CSV="$OUTDIR/play.csv"
PLAY_LOG="$OUTDIR/play.log"

echo "========================================================"
echo " Grace — Adaptive GR(1) Synthesis Pipeline"
echo "========================================================"
echo " Game        : $GAME_FILE"
echo " Mode        : $MODE"
echo " Output dir  : $OUTDIR"
echo " Binary      : $BIN"
[[ -n "$PARALLEL" ]] && echo " Parallel    : yes"
echo "========================================================"
echo ""

run_transform() {
    echo "[transform] Writing obliging arena and full spec..."
    "$BIN" "$GAME_FILE" transform --output "$OUTDIR"
    echo ""
}

run_synth() {
    local SYNTH_LOG="$OUTDIR/synth.log"
    echo "[synth] Running synthesis loop (BDD mode)..."
    echo "[synth] Output logged to: $SYNTH_LOG"
    local SYNTH_ARGS=(
        "$GAME_FILE" synth
        --fairsyn          "$FAIRSYN"
        --controller-query "$CTRL_QUERY"
        --owl-path         "$OWL_PATH"
        --workdir          "$OUTDIR"
        --ubdd-type        "$UBDD_TYPE"
    )
    if [[ -n "$PARALLEL"    ]]; then SYNTH_ARGS+=("$PARALLEL");              fi
    if [[ -n "$SKIP_DELETE" ]]; then SYNTH_ARGS+=("$SKIP_DELETE");           fi
    if [[ -n "$N_WORKERS"   ]]; then SYNTH_ARGS+=(--n-workers "$N_WORKERS"); fi
    "$BIN" "${SYNTH_ARGS[@]}" 2>&1 | tee "$SYNTH_LOG"
    echo ""
}

run_play() {
    #checking at least one BDD exists
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
    local PLAY_ARGS=(
        "$GAME_FILE" play
        --workdir          "$OUTDIR"
        --controller-query "$CTRL_QUERY"
        --steps            "$STEPS"
        --k                "$K"
        --alpha            "$ALPHA"
        --lambda           "$LAMBDA"
        --seed             "$SEED"
        --csv              "$CSV"
    )
    if [[ -n "$ALPHA_PRIME"  ]]; then PLAY_ARGS+=(--alpha-prime  "$ALPHA_PRIME");  fi
    if [[ -n "$LAMBDA_PRIME" ]]; then PLAY_ARGS+=(--lambda-prime "$LAMBDA_PRIME"); fi
    if [[ -n "$EPS"          ]]; then PLAY_ARGS+=(--eps          "$EPS");          fi
    if [[ -n "$TRACE"        ]]; then PLAY_ARGS+=(--trace        "$TRACE");        fi

    "$BIN" "${PLAY_ARGS[@]}" 2>&1 | tee "$PLAY_LOG"

    if [[ $PLOT -eq 1 && -f "$CSV" ]]; then
        local PLOT_OUT="$OUTDIR/play_plot.png"
        local PLOT_ARGS=("$CSV" --out "$PLOT_OUT")
        if [[ -n "$AINF" ]]; then PLOT_ARGS+=(--ainf "$AINF"); fi
        echo "[plot] Generating $PLOT_OUT ..."
        python3 "$SCRIPT_DIR/plot_policy.py" "${PLOT_ARGS[@]}" \
            && echo "[plot] Done." \
            || echo "[plot] Warning: plot failed (check plot_policy.py)" >&2
    fi

    echo ""
    echo "========================================================"
    echo " Play log : $PLAY_LOG"
    echo " CSV      : $CSV"
    if [[ $PLOT -eq 1 ]]; then echo " Plot     : $OUTDIR/play_plot.png"; fi
    echo "========================================================"
}

\case "$MODE" in
    transform) run_transform ;;
    synth)     run_synth     ;;
    play)      run_play      ;;
    all)       run_transform; run_synth; run_play ;;
esac