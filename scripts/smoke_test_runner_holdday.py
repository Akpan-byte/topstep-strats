#!/usr/bin/env python3
# CHANGE_SUMMARY
# 2026-08-19  kilo
#   - Created smoke test for the runner hold_day / hold_session fix.
#   - Verifies strategy 010 NYA tp=0.3 sl=8.0 across baseline, trail,
#     hold_day, and hold_session modes.
#   - Asserts no absurd losses (>$1M) and hold durations > 5 seconds.
# WHY: Quick regression test for the calendar-day boundary bug in hold_day.

"""Smoke test for runner hold-day/hold-session fix."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

import pandas as pd

from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import _atr, generate_signals, get_strategy_config
from scripts.runner_exit import apply_runner_to_signals as apply_runner_exit
from scripts.runner_exit_numba import apply_runner_to_signals as apply_runner_numba

DATA_DIR = _PROJECT_ROOT / "data"


def _run_mode(df, df_s, signals, mode_name, kwargs, point_value):
    runner_map = {
        "exit": apply_runner_exit,
        "numba": apply_runner_numba,
    }
    results = {}
    for impl_name, apply_runner in runner_map.items():
        if mode_name == "baseline":
            runner_signals = signals.copy()
        else:
            runner_signals = apply_runner(df_s, signals, **kwargs)

        bt = run_backtest(
            runner_signals,
            {"point_value": point_value, "topstep": {"enabled": True, "reset_on_failure": True}},
        )
        s = bt["summary"]
        results[impl_name] = {
            "total_pnl": float(s.get("cumulative_pnl_dollars", 0)),
            "executed_trades": int(s.get("executed_trades", 0)),
            "avg_hold_seconds": float(s.get("avg_hold_seconds", 0)),
            "median_hold_seconds": float(s.get("median_hold_seconds", 0)),
            "p10_hold_seconds": float(s.get("p10_hold_seconds", 0)),
            "p90_hold_seconds": float(s.get("p90_hold_seconds", 0)),
        }
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--instrument", default="NQ")
    parser.add_argument("--data-dir", default=str(DATA_DIR))
    args = parser.parse_args()

    sid = "010"
    instrument = args.instrument
    session = "NYA"
    tp = 0.3
    sl = 8.0
    point_value = 20.0

    cfg = get_strategy_config(sid)
    cfg.update({
        "instrument": instrument,
        "session": session,
        "session_start": "09:30",
        "session_end": "12:00",
        "tp_atr": tp,
        "sl_atr": sl,
        "session_only": True,
        "one_trade_per_day": True,
        "stop_first": True,
    })

    df = load_market_data(Path(args.data_dir) / f"{instrument}_1min.parquet")
    df["atr"] = _atr(df, 14)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")

    signals = generate_signals(df_s, cfg)
    print(f"Generated {len(signals)} signals for strategy {sid} {instrument} {session} tp={tp} sl={sl}")
    if signals.empty:
        print("FAIL: no signals generated")
        return 1

    modes = [
        ("baseline", {}),
        ("trail", {"mode": "trail", "trail_atr_mult": 1.0}),
        ("hold_day", {"mode": "hold_day", "day_end_time": pd.Timestamp("16:00").time()}),
        ("hold_session", {"mode": "hold_session", "session_end_time": pd.Timestamp("12:00").time()}),
    ]

    all_ok = True
    for mode_name, kwargs in modes:
        print(f"\n--- {mode_name} ---")
        results = _run_mode(df, df_s, signals, mode_name, kwargs, point_value)
        for impl_name, stats in results.items():
            print(
                f"  {impl_name}: pnl=${stats['total_pnl']:,.2f} "
                f"trades={stats['executed_trades']} "
                f"avg_hold={stats['avg_hold_seconds']:.1f}s "
                f"median={stats['median_hold_seconds']:.1f}s "
                f"p10={stats['p10_hold_seconds']:.1f}s "
                f"p90={stats['p90_hold_seconds']:.1f}s"
            )
            if stats["total_pnl"] < -1_000_000:
                print(f"    FAIL: absurd loss {stats['total_pnl']}")
                all_ok = False
            if stats["executed_trades"] > 0 and stats["avg_hold_seconds"] <= 5:
                print(f"    FAIL: avg hold time too short ({stats['avg_hold_seconds']}s)")
                all_ok = False
            if stats["executed_trades"] > 0 and stats["p10_hold_seconds"] <= 0:
                print(f"    FAIL: p10 hold time non-positive")
                all_ok = False

    print("\n" + ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
