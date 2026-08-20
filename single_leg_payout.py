# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created single_leg_payout.py.
#   - Loads top Paper-1 strategies, regenerates their trade-level history,
#     and runs each through topstep_payout.simulate_topstep_payouts for every
#     account spec and contract count.
#   - Reports net cash extracted (payouts) rather than gross equity growth.
# WHY: The user needs to know how much real money can be pulled out of
#      Topstep combines after resets, consistency rules, and payout caps.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pandas as pd

from stack_simulator import (
    DATA_DIR,
    DEFAULT_PORTFOLIO,
    POINT_VALUES,
    TICK_SIZES,
    generate_leg_trades,
    load_sweep_metadata,
)
from topstep_payout import TOPSTEP_SPECS, simulate_topstep_payouts


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Single-leg Topstep payout analysis.")
    parser.add_argument("--sweep-csv", default="/home/akpan/topstep-strats/gh_results/rust_sweep_v3/paper1_rust_sweep.csv")
    parser.add_argument("--output", default="/home/akpan/topstep-strats/gh_results/rust_sweep_v3/single_leg_payout.csv")
    parser.add_argument("--top-n", type=int, default=20)
    parser.add_argument("--start-date", default="2016-06-01")
    parser.add_argument("--end-date", default="2026-05-29")
    args = parser.parse_args(argv)

    sweep = load_sweep_metadata(Path(args.sweep_csv))
    # Focus on legs with decent win rate and positive weekly expectation.
    sweep = sweep[(sweep["win_rate"] >= 0.70) & (sweep["avg_per_week"] > 0)].copy()
    sweep = sweep.sort_values("avg_per_week", ascending=False).head(args.top_n)

    records = []

    for _, row in sweep.iterrows():
        leg = {
            "strategy_id": str(int(row["strategy_id"])),
            "instrument": row["instrument"],
            "session": row["session"],
            "tp": float(row["tp"]),
            "sl": float(row["sl"]),
            "mode": row["mode"],
        }
        label = f"{leg['strategy_id']:>3}_{leg['instrument']}_{leg['session']}_{leg['mode']}"
        print(f"Generating trades for {label} ...")
        trades = generate_leg_trades(leg, args.start_date, args.end_date)
        if trades.empty:
            print(f"  -> no trades, skipping")
            continue

        # Ensure required columns exist.
        trades = trades[["entry_time", "exit_time", "pnl", "instrument"]].copy()

        for spec_name, spec in TOPSTEP_SPECS.items():
            max_contracts = spec["max_contracts"]
            for contracts in range(1, max_contracts + 1):
                for cap_mode in (False, True):
                    result = simulate_topstep_payouts(
                        trades,
                        spec_name,
                        contracts=contracts,
                        daily_profit_cap_mode=cap_mode,
                    )
                    records.append({
                        "strategy_id": leg["strategy_id"],
                        "instrument": leg["instrument"],
                        "session": leg["session"],
                        "tp": leg["tp"],
                        "sl": leg["sl"],
                        "mode": leg["mode"],
                        "account": spec_name,
                        "contracts": contracts,
                        "daily_profit_cap": cap_mode,
                        "trades_executed": result["trades_executed"],
                        "win_rate": result["win_rate"],
                        "avg_trade_pnl": result["avg_trade_pnl"],
                        "total_payouts_dollars": result["total_payouts_dollars"],
                        "avg_payout_per_week": result["avg_payout_per_week"],
                        "avg_payout_per_month": result["avg_payout_per_month"],
                        "payouts_count": result["payouts_count"],
                        "resets": result["resets"],
                        "failed_combines": result["failed_combines"],
                        "daily_limit_hits": result["daily_limit_hits"],
                        "profit_cap_blocks": result["profit_cap_blocks"],
                        "consistency_violations": result["consistency_violations"],
                        "winning_days": result["winning_days"],
                        "max_drawdown_pct": result["max_drawdown_pct"],
                        "final_equity": result["final_equity"],
                    })

    out = pd.DataFrame(records)
    out = out.sort_values("avg_payout_per_week", ascending=False)
    out.to_csv(args.output, index=False)
    print(f"\nWrote {len(out)} rows to {args.output}")
    print("\nTop 10 by avg payout/week:")
    print(out.head(10)[["strategy_id", "instrument", "session", "tp", "sl", "mode",
                        "account", "contracts", "daily_profit_cap",
                        "avg_payout_per_week", "failed_combines", "payouts_count"]].to_string(index=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
