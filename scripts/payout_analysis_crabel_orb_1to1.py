#!/usr/bin/env python3
"""
Crabel ORB 1:1 payout analysis runner.

Runs the full 1:1 Crabel ORB configuration matrix over NQ/ES/YM RTH,
computes raw backtest metrics (no MC, no bootstrap), and simulates
Topstep combine payouts.  Designed to run on a remote worker (Akpan
laptop or GitHub Actions) so this VM does not carry the compute load.
"""

# CHANGE_SUMMARY
# 2026-08-23  kilo
#   - Added four experimental config columns to the row emitted by _run_config:
#     cancel_cutoff, require_setup, retracement_38_enabled, tic_volume_enabled.
# WHY: build_config_matrix() now produces 576 variant configs; the analysis CSV
#      must carry these flags so we can compare variant performance.

from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

import pandas as pd

# Ensure repo root is importable regardless of cwd.
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from topstep_strats.backtest import run_backtest
from topstep_strats.data import load_instrument_data
from topstep_strats.metrics import calculate_metrics
from topstep_strats.strategies.crabel_orb_1to1 import build_config_matrix, generate_signals
from topstep_payout import simulate_topstep_payouts, TOPSTEP_SPECS


# Realistic cost assumptions per round-turn trade (points).
COST_PER_TRADE_POINTS = {
    "NQ": 0.25 + 0.25,   # slippage 0.25 + commission approx 0.25 pt
    "ES": 0.25 + 0.10,
    "YM": 1.00 + 1.00,
}

POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}

TOPSTEP_ACCOUNTS = ["50k_standard", "150k_standard"]


def _run_config(cfg: Dict[str, Any], data_dir: Path) -> Dict[str, Any]:
    """Run one config through raw backtest + metrics + Topstep payout."""
    instrument = cfg["instrument"]
    df = load_instrument_data(instrument, str(data_dir))

    signals = generate_signals(df, cfg)
    if signals.empty:
        return {"strategy_id": cfg["strategy_id"], "instrument": instrument, "session": cfg["session"], "total_trades": 0}

    backtest_params = {
        "initial_capital": 100_000.0,
        "point_value": POINT_VALUES[instrument],
        "slippage": COST_PER_TRADE_POINTS[instrument] / 2.0,
        "commission": COST_PER_TRADE_POINTS[instrument] / 2.0,
    }

    raw_bt = run_backtest(signals, backtest_params)
    metrics = calculate_metrics(raw_bt, n_mc=0, n_boot=0)
    summary = raw_bt["summary"]

    row: Dict[str, Any] = {
        "strategy_id": cfg["strategy_id"],
        "instrument": instrument,
        "session": cfg["session"],
        "stretch_lookback": cfg["stretch_lookback"],
        "stretch_multiple": cfg["stretch_multiple"],
        "running_trend_sma": cfg["running_trend_sma"],
        "breakeven_fast_min": cfg["breakeven_fast_min"],
        "substantial_profit_multiple": cfg["substantial_profit_multiple"],
        "use_orbp": cfg["use_orbp"],
        "cancel_cutoff": cfg["cancel_cutoff"],
        "require_setup": cfg["require_setup"],
        "retracement_38_enabled": cfg["retracement_38_enabled"],
        "tic_volume_enabled": cfg["tic_volume_enabled"],
        "total_trades": summary["total_trades"],
        "executed_trades": summary["executed_trades"],
        "win_rate": summary["win_rate"],
        "net_pnl_points": summary["net_pnl_points"],
        "profit_factor": summary["profit_factor"],
        "max_drawdown_pct": summary["max_drawdown_pct"],
        "sharpe_ratio": metrics["basic"]["sharpe_ratio"],
        "cagr": metrics["basic"]["cagr"],
        "psr": metrics["probabilistic_sharpe_ratio"],
        "dsr": metrics["deflated_sharpe_ratio"],
    }

    # Topstep payout modeling for each account size / contract count.
    sig_for_payout = signals[["entry_time", "exit_time", "pnl", "instrument"]].copy()
    for account in TOPSTEP_ACCOUNTS:
        spec = TOPSTEP_SPECS[account]
        max_contracts = spec["max_contracts"]
        for contracts in range(1, max_contracts + 1):
            try:
                payout = simulate_topstep_payouts(sig_for_payout, account, contracts=contracts)
                prefix = f"{account}_c{contracts}"
                row[f"{prefix}_payouts"] = payout["payouts_count"]
                row[f"{prefix}_total_paid"] = payout["total_payouts_dollars"]
                row[f"{prefix}_resets"] = payout["resets"]
                row[f"{prefix}_winning_days"] = payout["winning_days"]
            except Exception as exc:
                prefix = f"{account}_c{contracts}"
                row[f"{prefix}_payouts"] = 0
                row[f"{prefix}_total_paid"] = 0.0
                row[f"{prefix}_resets"] = 0
                row[f"{prefix}_winning_days"] = 0
                row[f"{prefix}_error"] = str(exc)

    return row


def main() -> None:
    parser = argparse.ArgumentParser(description="Crabel ORB 1:1 payout sweep")
    parser.add_argument("--data-dir", type=Path, default=REPO_ROOT / "data", help="Path to NQ/ES/YM parquet files")
    parser.add_argument("--out-dir", type=Path, default=REPO_ROOT / "gh_results" / "crabel_orb_1to1", help="Output directory")
    parser.add_argument("--workers", type=int, default=os.cpu_count() or 1, help="Parallel workers")
    parser.add_argument("--instrument", type=str, default=None, help="Run a single instrument (NQ/ES/YM)")
    parser.add_argument("--smoke", action="store_true", help="Run only the first 3 configs")
    parser.add_argument("--mod", type=int, default=None, help="Shard modulus")
    parser.add_argument("--remainder", type=int, default=None, help="Shard remainder")
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)

    configs = build_config_matrix()
    if args.instrument:
        configs = [c for c in configs if c["instrument"] == args.instrument.upper()]
    if args.mod is not None and args.remainder is not None:
        configs = [c for i, c in enumerate(configs) if i % args.mod == args.remainder]
    if args.smoke:
        configs = configs[:3]

    print(f"[crabel-orb-1to1] Running {len(configs)} configs with {args.workers} workers")

    if args.workers > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=args.workers) as executor:
            results = list(executor.map(_worker, [(c, args.data_dir) for c in configs]))
    else:
        results = [_run_config(c, args.data_dir) for c in configs]

    df = pd.DataFrame(results)
    timestamp = datetime.utcnow().strftime("%Y%m%d_%H%M%S")
    csv_path = args.out_dir / f"crabel_orb_1to1_sweep_{timestamp}.csv"
    json_path = args.out_dir / f"crabel_orb_1to1_sweep_{timestamp}.json"
    df.to_csv(csv_path, index=False)
    df.to_json(json_path, orient="records", indent=2)

    print(f"[crabel-orb-1to1] Wrote {csv_path} ({len(df)} rows)")


def _worker(args_tuple: tuple) -> Dict[str, Any]:
    cfg, data_dir = args_tuple
    return _run_config(cfg, data_dir)


if __name__ == "__main__":
    main()
