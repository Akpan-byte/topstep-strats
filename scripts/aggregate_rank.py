# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created scripts/aggregate_rank.py to rank Paper-1 and Paper-2 sweep
#     results after filtering out HFT-flagged (<5s hold) and low-win-rate
#     (<80%) rows.
#   - Emits overall, per-instrument, and per-session top-N CSVs, plus a
#     JSON portfolio candidate built from up to 5 non-correlated strategies.
# WHY: Provides the final aggregation/ranking step for the matrix sweep
#      pipeline so the best diversified combine portfolio can be selected.

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd


PROJECT_ROOT = Path(__file__).resolve().parent.parent

REQUIRED_COLS = [
    "strategy_id",
    "instrument",
    "session",
    "tp",
    "sl",
    "mode",
    "win_rate",
    "executed_trades",
    "account_blowups",
    "daily_limit_hits",
    "max_drawdown_pct",
    "total_pnl_dollars",
    "avg_per_trade",
    "avg_per_week",
    "avg_per_day",
    "avg_hold_seconds",
    "median_hold_seconds",
]


def _coerce_numeric(df: pd.DataFrame, cols: List[str]) -> pd.DataFrame:
    """Convert listed columns to numeric, filling bad values with 0."""
    df = df.copy()
    for col in cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce").fillna(0)
    return df


def load_sweep_csvs(input_dir: Path) -> pd.DataFrame:
    """Load Paper-1 and Paper-2 sweep CSVs if present and tag the source."""
    sources = [
        ("paper1", input_dir / "paper1_comprehensive_sweep.csv"),
        ("paper2", input_dir / "paper2_focused_sweep.csv"),
    ]
    string_cols = {"strategy_id": str, "instrument": str, "session": str, "mode": str}
    frames: List[pd.DataFrame] = []
    for paper, path in sources:
        if not path.exists():
            print(f"[aggregate_rank] missing {path.name}, skipping", file=sys.stderr)
            continue
        df = pd.read_csv(path, dtype=string_cols)
        df["paper"] = paper
        frames.append(df)

    if not frames:
        raise RuntimeError(f"No sweep CSVs found in {input_dir}")

    combined = pd.concat(frames, ignore_index=True)
    missing = [c for c in REQUIRED_COLS if c not in combined.columns]
    if missing:
        raise ValueError(f"Sweep CSVs missing required columns: {missing}")

    return _coerce_numeric(combined, REQUIRED_COLS[6:])


def filter_rows(
    df: pd.DataFrame,
    min_hold_seconds: float = 5.0,
    min_win_rate: float = 0.80,
) -> pd.DataFrame:
    """Drop HFT-flagged and low-win-rate rows."""
    mask = pd.Series(True, index=df.index)
    if "avg_hold_seconds" in df.columns:
        mask &= df["avg_hold_seconds"] >= min_hold_seconds
    mask &= df["win_rate"] >= min_win_rate
    return df.loc[mask].copy()


def rank_overall(df: pd.DataFrame, n: int = 100) -> pd.DataFrame:
    """Return the top N rows ranked by avg_per_week descending."""
    ranked = df.sort_values(
        by=["avg_per_week", "win_rate", "executed_trades"],
        ascending=[False, False, False],
    ).reset_index(drop=True)
    ranked.insert(0, "rank", ranked.index + 1)
    return ranked.head(n)


def rank_by_group(df: pd.DataFrame, group_col: str, n: int = 20) -> pd.DataFrame:
    """Return the top N rows per group value, ranked by avg_per_week."""
    groups: List[pd.DataFrame] = []
    for value, group in df.groupby(group_col, sort=True):
        ranked = group.sort_values(
            by=["avg_per_week", "win_rate", "executed_trades"],
            ascending=[False, False, False],
        ).reset_index(drop=True)
        ranked.insert(0, f"{group_col}_rank", ranked.index + 1)
        groups.append(ranked.head(n))
    return pd.concat(groups, ignore_index=True) if groups else pd.DataFrame()


def _rows_correlated(a: pd.Series, b: pd.Series) -> bool:
    """Heuristic correlation: same strategy_id or same (instrument, session)."""
    if a["strategy_id"] == b["strategy_id"]:
        return True
    if a["instrument"] == b["instrument"] and a["session"] == b["session"]:
        return True
    return False


def find_portfolio_candidates(
    df: pd.DataFrame,
    max_size: int = 5,
    min_combined_weekly: float = 5_000.0,
) -> Optional[Dict[str, Any]]:
    """Build a small diversified portfolio from the ranked sweep results.

    Selection is greedy on ``avg_per_week`` while avoiding correlated pairs:
    two rows are considered correlated if they share the same strategy_id or
    the same (instrument, session) pair.  The returned candidate contains up
    to ``max_size`` strategies whose combined average weekly PnL is at least
    ``min_combined_weekly`` when possible.
    """
    ranked = df.sort_values(
        by=["avg_per_week", "win_rate", "executed_trades"],
        ascending=[False, False, False],
    )
    selected: List[pd.Series] = []
    for _, row in ranked.iterrows():
        if len(selected) >= max_size:
            break
        if all(not _rows_correlated(row, s) for s in selected):
            selected.append(row)

    if not selected:
        return None

    combined_weekly = float(sum(s["avg_per_week"] for s in selected))
    avg_weekly = combined_weekly / len(selected)
    win_rates = [float(s["win_rate"]) for s in selected]
    records: List[Dict[str, Any]] = []
    for s in selected:
        records.append(
            {
                "strategy_id": str(s["strategy_id"]),
                "instrument": str(s["instrument"]),
                "session": str(s["session"]),
                "tp": float(s["tp"]),
                "sl": float(s["sl"]),
                "mode": str(s["mode"]),
                "win_rate": float(s["win_rate"]),
                "avg_per_week": float(s["avg_per_week"]),
                "avg_per_trade": float(s["avg_per_trade"]),
                "executed_trades": int(s["executed_trades"]),
                "avg_hold_seconds": float(s["avg_hold_seconds"]),
                "paper": str(s.get("paper", "")),
            }
        )

    return {
        "strategies": records,
        "summary": {
            "count": len(selected),
            "combined_weekly": combined_weekly,
            "average_weekly": avg_weekly,
            "avg_win_rate": float(sum(win_rates) / len(win_rates)),
            "min_win_rate": min(win_rates),
            "threshold_met": combined_weekly >= min_combined_weekly,
            "min_combined_weekly": min_combined_weekly,
        },
    }


def _fmt_weekly(val: float) -> str:
    return f"${val:,.0f}"


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Aggregate and rank Paper-1/Paper-2 sweep results."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=PROJECT_ROOT / "gh_results",
        help="Directory containing paper1_comprehensive_sweep.csv and paper2_focused_sweep.csv",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=PROJECT_ROOT / "gh_results",
        help="Directory to write ranked CSVs and portfolio JSON",
    )
    parser.add_argument(
        "--min-hold-seconds",
        type=float,
        default=5.0,
        help="Minimum avg hold seconds to keep a row",
    )
    parser.add_argument(
        "--min-win-rate",
        type=float,
        default=0.80,
        help="Minimum win rate to keep a row",
    )
    parser.add_argument(
        "--min-combined-weekly",
        type=float,
        default=5_000.0,
        help="Combined weekly PnL threshold for the portfolio candidate",
    )
    args = parser.parse_args(argv)

    args.output_dir.mkdir(parents=True, exist_ok=True)

    df = load_sweep_csvs(args.input_dir)
    filtered = filter_rows(df, args.min_hold_seconds, args.min_win_rate)

    if filtered.empty:
        print("[aggregate_rank] no rows survive filters; nothing to output")
        return 0

    # Overall top 100.
    top_overall = rank_overall(filtered, n=100)
    overall_path = args.output_dir / "top_strategies_overall.csv"
    top_overall.to_csv(overall_path, index=False)
    print(f"[aggregate_rank] wrote {overall_path} ({len(top_overall)} rows)")

    # Per-instrument top 20.
    top_by_instrument = rank_by_group(filtered, "instrument", n=20)
    inst_path = args.output_dir / "top_strategies_by_instrument.csv"
    top_by_instrument.to_csv(inst_path, index=False)
    print(f"[aggregate_rank] wrote {inst_path} ({len(top_by_instrument)} rows)")

    # Per-session top 20.
    top_by_session = rank_by_group(filtered, "session", n=20)
    sess_path = args.output_dir / "top_strategies_by_session.csv"
    top_by_session.to_csv(sess_path, index=False)
    print(f"[aggregate_rank] wrote {sess_path} ({len(top_by_session)} rows)")

    # Portfolio candidate.
    portfolio = find_portfolio_candidates(
        filtered,
        min_combined_weekly=args.min_combined_weekly,
    )
    portfolio_path = args.output_dir / "portfolio_candidates.json"
    if portfolio is not None:
        with open(portfolio_path, "w") as f:
            json.dump(portfolio, f, indent=2)
        print(f"[aggregate_rank] wrote {portfolio_path}")
    else:
        portfolio_path.write_text(json.dumps({"strategies": [], "summary": {}}))
        print(f"[aggregate_rank] wrote empty {portfolio_path}")

    # Summary prints.
    print("\nTop 10 by weekly income:")
    print(
        top_overall.head(10)[[
            "rank",
            "strategy_id",
            "instrument",
            "session",
            "mode",
            "win_rate",
            "avg_per_week",
        ]]
        .assign(win_rate=lambda x: x["win_rate"].apply(lambda v: f"{v:.1%}"))
        .assign(avg_per_week=lambda x: x["avg_per_week"].apply(_fmt_weekly))
        .to_string(index=False)
    )

    print("\nBest portfolio candidate:")
    if portfolio is None or not portfolio["strategies"]:
        print("  (no qualifying diversified portfolio found)")
    else:
        summary = portfolio["summary"]
        print(f"  Strategies: {summary['count']}")
        print(f"  Combined weekly: {_fmt_weekly(summary['combined_weekly'])}")
        print(f"  Avg weekly:      {_fmt_weekly(summary['average_weekly'])}")
        print(f"  Avg win rate:    {summary['avg_win_rate']:.1%}")
        print(f"  Min win rate:    {summary['min_win_rate']:.1%}")
        print(f"  Threshold met:   {summary['threshold_met']}")
        for rec in portfolio["strategies"]:
            print(
                f"    {rec['strategy_id']:>6} {rec['instrument']} {rec['session']:<7} "
                f"{rec['mode']:<14} wr={rec['win_rate']:.1%} "
                f"weekly={_fmt_weekly(rec['avg_per_week'])}"
            )

    return 0


if __name__ == "__main__":
    sys.exit(main())
