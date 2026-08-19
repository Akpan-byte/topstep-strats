# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created scripts/aggregate_paper2.py as the paper2_sweep.yml finalizer.
#   - Reuses report-building helpers from aggregate.py so raw/topstep summaries
#     remain compatible with the topstep_parallel.yml pipeline.
#   - Adds per-instrument aggregation because each paper2 strategy ID is run on
#     ES/NQ/YM.
# WHY: aggregate.py already knows how to combine raw/topstep reports by strategy;
#      paper2 needs the same plus instrument-level breakdowns.

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import pandas as pd


# Reuse aggregate.py helpers; add scripts/ to the path so the module is importable
# regardless of the current working directory.
_SCRIPTS_DIR = Path(__file__).resolve().parent
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

import aggregate as _agg  # noqa: E402


def _build_report_for_mode(records, mode, out_dir):
    """Build the aggregate report for one mode, preserving the instrument column."""
    rows = []
    for rec in records:
        mode_rec = rec.get(mode, {}).copy()
        mode_rec["strategy"] = rec["strategy"]
        mode_rec["start_date"] = rec["start_date"]
        mode_rec["end_date"] = rec["end_date"]
        mode_rec["instrument"] = rec.get("params", {}).get("instrument", "unknown")
        rows.append(mode_rec)

    df = pd.DataFrame(rows)

    # Flatten nested metrics_summary dicts into top-level columns.
    metrics_rows = []
    for rec in records:
        metrics_rows.append(rec.get(mode, {}).get("metrics_summary", {}))
    if any(metrics_rows):
        metrics_df = pd.DataFrame(metrics_rows)
        for col in metrics_df.columns:
            df[col] = metrics_df[col].values

    for col in _agg.NUMERIC_COLS:
        if col not in df.columns:
            df[col] = 0.0
        else:
            df[col] = pd.to_numeric(df[col], errors="coerce").fillna(0.0)

    for col in ["account_failed", "profit_target_reached"]:
        if col not in df.columns:
            df[col] = False
        else:
            df[col] = df[col].astype(bool)

    return {
        "overall": _agg._aggregate_group(df),
        "by_strategy": {
            strategy: _agg._aggregate_group(group)
            for strategy, group in df.groupby("strategy", sort=True)
        },
        "by_instrument": {
            instrument: _agg._aggregate_group(group)
            for instrument, group in df.groupby("instrument", sort=True)
        },
        "chart_files": [_agg._make_chart(df, out_dir, mode)],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Aggregate paper2_sweep chunk JSONs into a final report."
    )
    parser.add_argument("--input", required=True, help="Directory containing chunk JSON files")
    parser.add_argument("--output", required=True, help="Directory to write final_report.json and chart")
    args = parser.parse_args(argv)

    input_dir = Path(args.input)
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    records = _agg._load_records(input_dir)
    if not records:
        raise RuntimeError(f"No valid chunk JSON files found in {input_dir}")

    report = {
        "raw": _build_report_for_mode(records, "raw", output_dir),
        "topstep": _build_report_for_mode(records, "topstep", output_dir),
        "chunks": records,
    }

    report_path = output_dir / "final_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, default=str)

    print(f"[aggregate_paper2] wrote {report_path}")


if __name__ == "__main__":
    main()
