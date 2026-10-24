#!/usr/bin/env bash
# batch_play.sh — Run the play phase for multiple Adam traces, each with a
# random seed, storing results in a structured output directory
#
# Assumes synthesis has already been run (workdir contains controller.bdd files)
#
# Usage:
#   ./script/batch_play.sh benchmarks/<name>/arena.txt <trace1> [trace2 ...] [options]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CTRL_QUERY="${GRACE_CTRL_QUERY:-$ROOT/third_party/fairsyn/build/bin/controller_query}"


STEPS=500
K=1
ALPHA=0.5
ALPHA_PRIME=""
LAMBDA=0.9
LAMBDA_PRIME=""
EPS=""
AINF=""
PLOT=0
RUNS=1
WORKDIR=""
OUTDIR=""
GAME_FILE=""
TRACES=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workdir)      WORKDIR="$2";       shift 2 ;;
        --outdir)       OUTDIR="$2";        shift 2 ;;
        --steps)        STEPS="$2";         shift 2 ;;
        --k)            K="$2";             shift 2 ;;
        --alpha)        ALPHA="$2";         shift 2 ;;
        --alpha-prime)  ALPHA_PRIME="$2";   shift 2 ;;
        --lambda)       LAMBDA="$2";        shift 2 ;;
        --lambda-prime) LAMBDA_PRIME="$2";  shift 2 ;;
        --eps)          EPS="$2";           shift 2 ;;
        --ainf)         AINF="$2";          shift 2 ;;
        --ctrl-query)   CTRL_QUERY="$2";    shift 2 ;;
        --runs)         RUNS="$2";          shift 2 ;;
        --plot)         PLOT=1;             shift   ;;
        --help|-h)
            sed -n '2,40p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        -*)  echo "[error] Unknown option: $1" >&2; exit 1 ;;
        *)
            if [[ -z "$GAME_FILE" ]]; then
                GAME_FILE="$1"
            else
                TRACES+=("$1")
            fi
            shift ;;
    esac
done

# Locate compiled binary
if [[ -x "$ROOT/build/bin/grace" ]]; then BIN="$ROOT/build/bin/grace"
elif [[ -x "$ROOT/src/main" ]]; then BIN="$ROOT/src/main"
else echo "[error] Cannot find grace binary. Run ./build/build_all.sh first." >&2; exit 1; fi

[[ -z "$GAME_FILE" ]] && { echo "[error] No game file specified." >&2; exit 1; }
[[ ${#TRACES[@]} -eq 0 ]] && { echo "[error] No trace files specified." >&2; exit 1; }
[[ ! -f "$GAME_FILE" ]] && { echo "[error] Game file not found: $GAME_FILE" >&2; exit 1; }
[[ ! -x "$CTRL_QUERY" ]] && { echo "[error] controller_query not found: $CTRL_QUERY" >&2; exit 1; }

GAME_DIR="$(cd "$(dirname "$GAME_FILE")" && pwd)"
if [[ "$(basename "$GAME_FILE")" == "arena.txt" ]]; then
    WORKDIR="${WORKDIR:-$GAME_DIR/out}"
    OUTDIR="${OUTDIR:-$GAME_DIR/batch_results}"
else
    STEM="$(basename "$GAME_FILE" .txt)"
    WORKDIR="${WORKDIR:-$GAME_DIR/$STEM}"
    OUTDIR="${OUTDIR:-$GAME_DIR/batch_results/$STEM}"
fi
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
WORKDIR="$(cd "$WORKDIR" && pwd)"

# Verify synthesis output exists
found_bdd=0
for d in "$WORKDIR"/A*/; do
    [[ -f "$d/controller.bdd" ]] && { found_bdd=1; break; }
done
[[ $found_bdd -eq 0 ]] && {
    echo "[error] No controller.bdd found in $WORKDIR — run synthesis first." >&2; exit 1; }

echo "========================================================"
echo " Grace — Batch Play Phase"
echo "========================================================"
echo " Game      : $GAME_FILE"
echo " Workdir   : $WORKDIR"
echo " Outdir    : $OUTDIR"
echo " Traces    : ${#TRACES[@]}"
echo " Runs/trace: $RUNS"
echo " Steps     : $STEPS"
echo " k / α / λ : $K / $ALPHA / $LAMBDA"
if [[ -n "$AINF" ]]; then echo " A_inf     : $AINF"; fi
echo "========================================================"

TOTAL_RUNS=$(( ${#TRACES[@]} * RUNS ))
COMPLETED=0
FAILED=0

for TRACE in "${TRACES[@]}"; do
    if [[ ! -f "$TRACE" ]]; then
        echo "[warn] Trace file not found, skipping: $TRACE" >&2
        continue
    fi

    TRACE_ABS="$(realpath "$TRACE")"
    TRACE_STEM="$(basename "$TRACE" .txt)"

    for ((run=1; run<=RUNS; run++)); do
        # Random seed: combine $RANDOM twice for a wider range
        SEED=$(( (RANDOM << 15) | RANDOM ))
        RUN_DIR="$OUTDIR/${TRACE_STEM}_run${run}"
        mkdir -p "$RUN_DIR"

        CSV="$RUN_DIR/play.csv"
        LOG="$RUN_DIR/play.log"
        echo "$SEED" > "$RUN_DIR/seed.txt"

        echo ""
        echo "[run $((COMPLETED+1))/$TOTAL_RUNS] trace=$(basename "$TRACE") run=$run seed=$SEED"

        PLAY_ARGS=(
            "$GAME_FILE" play
            --workdir           "$WORKDIR"
            --controller-query  "$CTRL_QUERY"
            --steps             "$STEPS"
            --k                 "$K"
            --alpha             "$ALPHA"
            --lambda            "$LAMBDA"
            --seed              "$SEED"
            --trace             "$TRACE_ABS"
            --csv               "$CSV"
        )
        if [[ -n "$ALPHA_PRIME"  ]]; then PLAY_ARGS+=(--alpha-prime  "$ALPHA_PRIME");  fi
        if [[ -n "$LAMBDA_PRIME" ]]; then PLAY_ARGS+=(--lambda-prime "$LAMBDA_PRIME"); fi
        if [[ -n "$EPS"          ]]; then PLAY_ARGS+=(--eps          "$EPS");          fi

        if "$BIN" "${PLAY_ARGS[@]}" > "$LOG" 2>&1; then
            echo "  [ok]  csv: $CSV"
        else
            echo "  [fail] exit $? — see $LOG" >&2
            FAILED=$((FAILED+1))
        fi
        COMPLETED=$((COMPLETED+1))

        if [[ $PLOT -eq 1 && -f "$CSV" ]]; then
            PLOT_ARGS=("$CSV" --out "$RUN_DIR/play_plot.png")
            if [[ -n "$AINF" ]]; then PLOT_ARGS+=(--ainf "$AINF"); fi
            if python3 "$SCRIPT_DIR/plot_policy.py" "${PLOT_ARGS[@]}" > "$RUN_DIR/plot.out" 2>&1; then
                echo "  [plot] $RUN_DIR/play_plot.png"
                if [[ -n "$AINF" ]]; then
                    grep '^\[rq2\]' "$RUN_DIR/plot.out" | tee "$RUN_DIR/rq2.txt" | sed 's/^/  /' || true
                fi
            else
                echo "  [plot] failed — see $RUN_DIR/plot.out" >&2
            fi
        fi
    done
done

echo ""
echo " Done. $((COMPLETED-FAILED))/$TOTAL_RUNS runs succeeded."
echo " Results in: $OUTDIR"