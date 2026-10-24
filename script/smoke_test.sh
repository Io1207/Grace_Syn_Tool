#!/usr/bin/env bash
# smoke_test.sh — End-to-end check (synthesis + 100 play steps) on a small
#
# Usage: ./script/smoke_test.sh [game_file]     (default: benchmarks/fig2/arena.txt)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # artifact root
GAME="${1:-$ROOT/benchmarks/matching_pennies/arena.txt}"
OUT="$(mktemp -d)/smoke"

fail() { echo "[FAIL] $*" >&2; exit 1; }

[[ -f "$GAME" ]] || fail "benchmark not found: $GAME"

echo "[smokeTest] game   : $GAME"
echo "[smokeTest] output : $OUT"
"$ROOT/script/run_pipeline.sh" "$GAME" --mode all --steps 500 --outdir "$OUT" "}

[[ -s "$OUT/winning_vertices.txt" ]] || fail "W was not computed (winning_vertices.txt missing/empty)"
ls "$OUT"/A*/controller.bdd >/dev/null 2>&1 || fail "no subgame strategies were stored"
grep -q "\[error\]" "$OUT/synth.log" && fail "errors in synth.log — see $OUT/synth.log"
[[ -f "$OUT/play.csv" ]] || fail "play.csv missing"
rows=$(( $(wc -l < "$OUT/play.csv") - 1 ))
(( rows >= 500 )) || fail "play stopped early ($rows/500 steps) — see $OUT/play.log"
grep -q "\[error\]" "$OUT/play.log" && fail "errors in play.log — see $OUT/play.log"

if grep -q "q convention mismatch" "$OUT/play.log"; then
    echo "[warn] automaton-state convention mismatch reported — see $OUT/play.log"
fi

echo "[smokeTest] PASS — $rows play steps, outputs in $OUT"