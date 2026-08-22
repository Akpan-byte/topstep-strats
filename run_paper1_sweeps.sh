#!/usr/bin/env bash
set -euo pipefail

PROJECT=/home/akpan/topstep-strats
VENV="$PROJECT/.venv312/bin/activate"
SCRIPT="$PROJECT/engine_rust/scripts/sweep_engine.py"
OUTDIR="$PROJECT/gh_results/rust_sweep_v4"
LOG="$OUTDIR/paper1_sweeps.log"

cd "$PROJECT"
. "$VENV"
mkdir -p "$OUTDIR"

exec > >(tee -a "$LOG") 2>&1

echo "=== Paper-1 50k sweep start: $(date -Iseconds) ==="
python "$SCRIPT" \
  --paper paper1 \
  --workers 4 \
  --topstep \
  --account-size 50k \
  --output-dir "$OUTDIR" \
  --overwrite \
  --flush-every 1000

echo "=== Paper-1 50k sweep finished: $(date -Iseconds) ==="
echo "=== Paper-1 150k sweep start: $(date -Iseconds) ==="
python "$SCRIPT" \
  --paper paper1 \
  --workers 4 \
  --topstep \
  --account-size 150k \
  --output-dir "$OUTDIR" \
  --overwrite \
  --flush-every 1000

echo "=== Paper-1 150k sweep finished: $(date -Iseconds) ==="
