#!/usr/bin/env python3
"""Aggregate sweep CSVs and emit ranked top-performer tables.

Usage
-----
    python engine_rust/scripts/aggregate_results.py \
        --paper1 gh_results/rust_sweep_v2/paper1_rust_sweep.csv \
        --paper2 gh_results/rust_sweep_v2/paper2_rust_sweep.csv \
        --output-dir gh_results/rust_sweep_v2
"""
from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd


def load(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    df["avg_per_week"] = pd.to_numeric(df["avg_per_week"], errors="coerce")
    df["win_rate"] = pd.to_numeric(df["win_rate"], errors="coerce")
    df["n_trades"] = pd.to_numeric(df["n_trades"], errors="coerce")
    df["trades_per_day"] = pd.to_numeric(df.get("trades_per_day", 0.0), errors="coerce")
    df["total_pnl_dollars"] = pd.to_numeric(df["total_pnl_dollars"], errors="coerce")
    df["avg_per_trade"] = pd.to_numeric(df["avg_per_trade"], errors="coerce")
    return df


def rank_paper1(df: pd.DataFrame) -> pd.DataFrame:
    return df.sort_values(["avg_per_week", "win_rate"], ascending=[False, False])


def rank_paper2(df: pd.DataFrame) -> pd.DataFrame:
    return df.sort_values(["avg_per_week", "win_rate"], ascending=[False, False])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--paper1", required=True)
    parser.add_argument("--paper2", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    out = Path(args.output_dir)
    out.mkdir(parents=True, exist_ok=True)

    p1 = load(args.paper1)
    p2 = load(args.paper2)

    # Paper-1 top tables
    p1_ranked = rank_paper1(p1)
    p1_ranked.head(100).to_csv(out / "paper1_top100_pnl.csv", index=False)
    p1[p1["win_rate"] >= 0.80].sort_values("avg_per_week", ascending=False).head(100).to_csv(
        out / "paper1_top100_win80.csv", index=False
    )
    p1[p1["win_rate"] >= 0.70].sort_values("avg_per_week", ascending=False).head(100).to_csv(
        out / "paper1_top100_win70.csv", index=False
    )
    p1[p1["win_rate"] >= 0.90].sort_values("avg_per_week", ascending=False).head(100).to_csv(
        out / "paper1_top100_win90.csv", index=False
    )

    # Paper-2 top tables
    p2_ranked = rank_paper2(p2)
    p2_ranked.head(100).to_csv(out / "paper2_top100_pnl.csv", index=False)
    p2[p2["win_rate"] >= 0.55].sort_values("avg_per_week", ascending=False).head(100).to_csv(
        out / "paper2_top100_win55.csv", index=False
    )

    # Cross-paper combined view
    combined = pd.concat(
        [
            p1[["paper", "strategy_id", "instrument", "session", "tp", "sl", "mode", "win_rate", "n_trades", "trades_per_day", "avg_per_week", "total_pnl_dollars"]],
            p2[["paper", "strategy_id", "instrument", "session", "risk_profile", "win_rate", "n_trades", "trades_per_day", "avg_per_week", "total_pnl_dollars"]].rename(columns={"risk_profile": "mode"}),
        ],
        ignore_index=True,
    )
    combined.sort_values("avg_per_week", ascending=False).head(200).to_csv(out / "combined_top200.csv", index=False)

    print(f"Wrote aggregated tables to {out}")
    print(f"Paper-1 rows: {len(p1)} | Paper-2 rows: {len(p2)}")
    print("\nTop 10 by avg/week:")
    print(combined.sort_values("avg_per_week", ascending=False).head(10).to_string(index=False))


if __name__ == "__main__":
    main()
