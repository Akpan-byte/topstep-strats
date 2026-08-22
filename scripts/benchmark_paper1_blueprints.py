#!/usr/bin/env python3
"""Benchmark generate_signals for each Paper-1 blueprint on local NQ data.

Runs at least 3 strategy IDs per blueprint and reports average seconds per call.
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import defaultdict
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

import pandas as pd

from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import (
    BLUEPRINTS,
    generate_signals,
    get_strategy_config,
    get_matrix_df,
)

SESSION = "NY"
SESSION_START = "09:30"
SESSION_END = "16:00"


def _strategy_ids_for_blueprint(blueprint: str, n: int = 3) -> list[str]:
    df = get_matrix_df()
    ids = df.loc[df["blueprint"] == blueprint, "id"].tolist()[:n]
    return ids


def main():
    parser = argparse.ArgumentParser(description="Benchmark Paper-1 blueprints")
    parser.add_argument("--data-dir", type=str, default=str(_PROJECT_ROOT / "data"))
    parser.add_argument("--start-date", type=str, default="2016-06-01")
    parser.add_argument("--end-date", type=str, default="2026-05-29")
    parser.add_argument("--per-blueprint", type=int, default=3)
    parser.add_argument("--instrument", type=str, default="NQ")
    args = parser.parse_args()

    df = load_market_data(Path(args.data_dir) / f"{args.instrument}_1min.parquet")
    mask = get_session_mask(df, SESSION_START, SESSION_END, "America/New_York")
    df_s = split_by_date(df.loc[mask].copy(), args.start_date, args.end_date)
    print(f"Data rows: {len(df_s):,}, unique dates: {len(set(df_s.index.date)):,}")

    results: list[dict] = []
    for bp in BLUEPRINTS:
        sids = _strategy_ids_for_blueprint(bp, args.per_blueprint)
        if not sids:
            print(f"No IDs for {bp}")
            continue
        times = []
        n_trades = []
        for sid in sids:
            cfg = get_strategy_config(sid)
            cfg.update(
                {
                    "instrument": args.instrument,
                    "session": SESSION,
                    "session_start": SESSION_START,
                    "session_end": SESSION_END,
                }
            )
            t0 = time.perf_counter()
            sig = generate_signals(df_s, cfg)
            elapsed = time.perf_counter() - t0
            times.append(elapsed)
            n_trades.append(len(sig))
            results.append(
                {
                    "blueprint": bp,
                    "strategy_id": sid,
                    "seconds": elapsed,
                    "trades": len(sig),
                }
            )
        avg = sum(times) / len(times)
        print(
            f"{bp:30s} avg={avg:6.2f}s  ids={sids}  times={[f'{t:.2f}' for t in times]}  trades={n_trades}"
        )

    out = Path(args.data_dir) / ".." / "gh_results" / "paper1_benchmark_before.csv"
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    pd.DataFrame(results).to_csv(out, index=False)
    print(f"Wrote raw timings to {out}")


if __name__ == "__main__":
    main()
