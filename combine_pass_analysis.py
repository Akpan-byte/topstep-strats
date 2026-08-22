# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created combine_pass_analysis.py.
#   - Loads Paper-1 sweep results, regenerates trade-level history per strategy,
#     and runs each through combine_pass.simulate_combine_passes.
#   - Evaluates combine_50k (1-5 contracts) and combine_150k (1-10 contracts)
#     under both EOD and trailing daily-drawdown modes.
#   - Uses multiprocessing for speed.
# WHY: User needs time-to-pass statistics (mean/median/cluster/min/max) for
#      every strategy under proper combine evaluation rules.

from __future__ import annotations

import argparse
import sys
from functools import partial
from pathlib import Path
from typing import Any, Dict, List

import pandas as pd
from multiprocessing import Pool, cpu_count

from combine_pass import COMBINE_SPECS, simulate_combine_passes
from stack_simulator import load_sweep_metadata, generate_leg_trades


def _run_one(cfg: Dict[str, Any], start_date: str, end_date: str) -> List[Dict[str, Any]]:
    """Generate trades for one strategy config and run all combine variants."""
    leg = {
        "strategy_id": str(int(cfg["strategy_id"])),
        "instrument": cfg["instrument"],
        "session": cfg["session"],
        "tp": float(cfg["tp"]),
        "sl": float(cfg["sl"]),
        "mode": cfg["mode"],
    }
    try:
        trades = generate_leg_trades(leg, start_date, end_date)
    except Exception as e:
        print(f"ERROR generating trades for {leg}: {e}")
        return []
    if trades.empty:
        return []

    trades = trades[["entry_time", "exit_time", "pnl", "instrument"]].copy()

    records = []
    for account in ["combine_50k", "combine_150k"]:
        max_contracts = COMBINE_SPECS[account]["max_contracts"]
        for contracts in range(1, max_contracts + 1):
            for mode in ("eod", "trailing"):
                result = simulate_combine_passes(
                    trades, account, contracts=contracts, daily_dd_mode=mode
                )
                if result["combines_simulated"] == 0:
                    continue
                records.append({
                    "strategy_id": leg["strategy_id"],
                    "instrument": leg["instrument"],
                    "session": leg["session"],
                    "tp": leg["tp"],
                    "sl": leg["sl"],
                    "mode": leg["mode"],
                    "account": account,
                    "contracts": contracts,
                    "daily_dd_mode": mode,
                    "pass_rate": result["pass_rate"],
                    "combines_simulated": result["combines_simulated"],
                    "passed_combines": result["passed_combines"],
                    "failed_combines": result["failed_combines"],
                    "avg_days_to_pass": result["avg_days_to_pass"],
                    "median_days_to_pass": result["median_days_to_pass"],
                    "min_days_to_pass": result["min_days_to_pass"],
                    "max_days_to_pass": result["max_days_to_pass"],
                    "p50_days_to_pass": result["p50_days_to_pass"],
                    "p75_days_to_pass": result["p75_days_to_pass"],
                    "p90_days_to_pass": result["p90_days_to_pass"],
                    "p95_days_to_pass": result["p95_days_to_pass"],
                    "p99_days_to_pass": result["p99_days_to_pass"],
                    "avg_trades_to_pass": result["avg_trades_to_pass"],
                    "median_trades_to_pass": result["median_trades_to_pass"],
                    "avg_max_drawdown_pct": result["avg_max_drawdown_pct"],
                })
    return records


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Combine-pass analysis across Paper-1 strategies.")
    parser.add_argument("--sweep-csv", default="/home/akpan/topstep-strats/gh_results/rust_sweep_v3/paper1_rust_sweep.csv")
    parser.add_argument("--output", default="/home/akpan/topstep-strats/gh_results/rust_sweep_v3/combine_pass_analysis.csv")
    parser.add_argument("--top-n", type=int, default=None, help="Limit to top N by avg_per_week (default: all)")
    parser.add_argument("--min-win-rate", type=float, default=0.65)
    parser.add_argument("--workers", type=int, default=None)
    parser.add_argument("--start-date", default="2016-06-01")
    parser.add_argument("--end-date", default="2026-05-29")
    args = parser.parse_args(argv)

    sweep = load_sweep_metadata(Path(args.sweep_csv))
    if args.min_win_rate:
        sweep = sweep[sweep["win_rate"] >= args.min_win_rate].copy()
    if args.top_n:
        sweep = sweep.sort_values("avg_per_week", ascending=False).head(args.top_n)

    # Drop duplicate strategy configs (same ID/instrument/session/tp/sl/mode).
    sweep = sweep.drop_duplicates(subset=["strategy_id", "instrument", "session", "tp", "sl", "mode"])
    configs = sweep.to_dict("records")
    print(f"Running combine-pass analysis on {len(configs)} unique strategy configs")

    workers = args.workers or max(1, cpu_count() - 1)
    runner = partial(_run_one, start_date=args.start_date, end_date=args.end_date)

    all_records: List[Dict[str, Any]] = []
    with Pool(workers) as pool:
        for i, records in enumerate(pool.imap_unordered(runner, configs), 1):
            all_records.extend(records)
            if i % 10 == 0:
                print(f"  processed {i}/{len(configs)} configs, {len(all_records)} rows so far")

    out = pd.DataFrame(all_records)
    if not out.empty:
        # Sort by fastest median time-to-pass among passing combines.
        out = out.sort_values(["pass_rate", "median_days_to_pass"], ascending=[False, True])
    out.to_csv(args.output, index=False)
    print(f"\nWrote {len(out)} rows to {args.output}")
    print("\nTop 10 fastest passing combines:")
    print(out.head(10)[["strategy_id", "instrument", "session", "tp", "sl", "mode",
                        "account", "contracts", "daily_dd_mode", "pass_rate",
                        "median_days_to_pass", "avg_days_to_pass", "failed_combines"]].to_string(index=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
