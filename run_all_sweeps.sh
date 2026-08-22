#!/bin/bash
set -e
cd /home/akpan/topstep-strats
source .venv312/bin/activate
OUT=gh_results/rust_sweep_v3
mkdir -p "$OUT"

echo "=== Paper-1 sweep ==="
python engine_rust/scripts/sweep_engine.py --paper paper1 --workers 16 --output-dir "$OUT" --topstep --overwrite

echo "=== Paper-2 sweep ==="
python engine_rust/scripts/sweep_engine.py --paper paper2 --workers 16 --output-dir "$OUT" --topstep --overwrite

echo "=== Portfolio1 sweep ==="
python engine_rust/scripts/sweep_engine.py --paper portfolio1 --workers 16 --output-dir "$OUT" --topstep --overwrite

echo "=== Aggregate ==="
python engine_rust/scripts/aggregate_results.py --paper1 "$OUT/paper1_rust_sweep.csv" --paper2 "$OUT/paper2_rust_sweep.csv" --output-dir "$OUT" > "$OUT/aggregate.log"

echo "=== Optimizer ==="
python engine_rust/scripts/optimizer.py "$OUT/paper1_rust_sweep.csv" --output "$OUT/optimizer_candidates.csv" --min-weekly 5000 --min-win-rate 0.80

echo "ALL_DONE"
