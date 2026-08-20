#!/usr/bin/env python3
"""Validate that Rust run_signals now matches the Python Topstep semantics.

Compares strategy 007 (NQ NYP Casper inverted FVG) run through:
1. Python run_backtest with Topstep reset_on_failure.
2. Rust AgnosticBacktest.run_signals with Topstep reset_on_failure.
"""
from __future__ import annotations

import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))
sys.path.insert(0, str(_PROJECT_ROOT / "engine_rust" / "python"))

import numpy as np
import pandas as pd

from agnostic_backtest import AgnosticBacktest, TopstepConfig
from scripts.runner_exit_numba import apply_runner_to_signals
from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import (
    generate_signals as generate_paper1_signals,
    get_strategy_config as get_paper1_config,
)


def attach_tp_sl(entry_signals: pd.DataFrame, tp: float, sl: float, tick_size: float) -> pd.DataFrame:
    signals = entry_signals.copy()
    atr = signals["atr_value"].values
    direction = signals["direction"].values
    entry_price = signals["entry_price"].values
    signals["stop_loss"] = np.where(
        direction == 1,
        entry_price - sl * atr - tick_size,
        entry_price + sl * atr + tick_size,
    )
    signals["take_profit"] = np.where(
        direction == 1,
        entry_price + tp * atr + tick_size,
        entry_price - tp * atr - tick_size,
    )
    return signals


def main() -> None:
    data_dir = _PROJECT_ROOT / "data"
    df = load_market_data(str(data_dir / "NQ_1min.parquet"))
    df["atr"] = generate_paper1_signals.__module__
    # compute atr using the helper from paper1_matrix
    from topstep_strats.strategies.paper1_matrix import _atr

    df["atr"] = _atr(df, 14)

    sid = "007"
    session = "NY"
    start = "2016-06-01"
    end = "2026-05-29"
    tp = 0.1
    sl = 5.0

    cfg = get_paper1_config(sid)
    cfg.update(
        {
            "instrument": "NQ",
            "tick_size": 0.25,
            "point_value": 20.0,
            "session": session,
            "session_start": "09:30",
            "session_end": "16:00",
            "tp_atr": tp,
            "sl_atr": sl,
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )

    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start, end)

    # Python legacy path: generate_signals with simulate_exits=True
    py_signals = generate_paper1_signals(df_s, cfg, simulate_exits=True)
    py_result = run_backtest(
        py_signals,
        params={
            "initial_capital": 100_000.0,
            "point_value": 20.0,
            "topstep": {
                "enabled": True,
                "account_size": 50_000.0,
                "daily_drawdown_limit": 900.0,
                "trailing_drawdown_limit": 2_000.0,
                "profit_target": 3_000.0,
                "reset_on_failure": True,
            },
        },
    )
    py_summary = py_result["summary"]

    # Rust path: entry-only signals + attach TP/SL + runner + Rust engine
    entry_signals = generate_paper1_signals(df_s, cfg, simulate_exits=False)
    signals_with_sl_tp = attach_tp_sl(entry_signals, tp, sl, 0.25)
    runner_signals = apply_runner_to_signals(
        df_s, signals_with_sl_tp, mode="baseline", session_end_time=pd.Timestamp(cfg["session_end"]).time()
    )

    print("sample entry_ts:", runner_signals["entry_time"].head().tolist())
    print("sample entry_ts ns:", pd.to_datetime(runner_signals["entry_time"].head(), utc=True).astype("int64").tolist())

    engine = AgnosticBacktest(cash=100_000.0, point_value=20.0)
    rust_summary = engine.run_signals(
        runner_signals,
        topstep=TopstepConfig(
            enabled=True,
            daily_drawdown_limit=900.0,
            trailing_drawdown_limit=2_000.0,
            profit_target=3_000.0,
            reset_on_failure=True,
        ),
    )

    print("=" * 60)
    print(f"Strategy {sid} NQ {session} TP={tp} SL={sl}")
    print("-" * 60)
    print(f"Python  n_trades={py_summary['executed_trades']}  win_rate={py_summary['win_rate']:.4f}  total_pnl={py_summary['cumulative_pnl_dollars']:.2f}")
    print(f"        blowups={py_summary['account_blowups']}  daily_hits={py_summary['daily_limit_hits']}  trailing_hits={py_summary['trailing_limit_hits']}  profit_target={py_summary['profit_target_reached']}")
    py_reasons = py_signals["exit_reason"].value_counts().to_dict()
    print(f"        reasons={py_reasons}")
    print(f"Rust    n_trades={rust_summary.executed_trades}  win_rate={rust_summary.win_rate:.4f}  total_pnl={rust_summary.total_pnl:.2f}")
    rust_reasons = runner_signals["exit_reason"].value_counts().to_dict()
    print(f"        reasons={rust_reasons}  blowups={rust_summary.account_blowups}  daily_hits={rust_summary.daily_limit_hits}  trailing_hits={rust_summary.trailing_limit_hits}  profit_target={rust_summary.profit_target_reached}")
    print(f"        runner_signals unique days = {runner_signals['entry_time'].dt.normalize().nunique()}")
    print("=" * 60)

    # Allow small differences due to commission/slippage defaults (both zero here).
    assert py_summary["executed_trades"] == rust_summary.executed_trades, "trade count mismatch"
    assert abs(py_summary["win_rate"] - rust_summary.win_rate) < 0.01, "win rate mismatch"
    assert abs(py_summary["cumulative_pnl_dollars"] - rust_summary.total_pnl) < 1.0, "PnL mismatch"
    print("VALIDATION PASSED")


if __name__ == "__main__":
    main()
