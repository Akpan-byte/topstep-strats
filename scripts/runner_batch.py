#!/usr/bin/env python3
"""Run a slice of the TP-to-runner sweep for GitHub Actions parallel workers.

The full parameter grid is the cross product of BASE_RUNS (top Paper-1 Casper
NQ settings) and RUNNER_MODES (baseline, trail-*ATR, hold_day, hold_session).
Each worker handles combos where ``index % n_batches == batch_id``.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

import pandas as pd

from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import get_strategy_config, generate_signals, _atr
from scripts.runner_exit import apply_runner_to_signals

POINT_VALUE = 20.0

BASE_RUNS = [
    {"id": "010", "label": "NYA",  "session_start": "09:30", "session_end": "12:00", "tp": 0.5, "sl": 8.0},
    {"id": "010", "label": "NYA",  "session_start": "09:30", "session_end": "12:00", "tp": 0.5, "sl": 10.0},
    {"id": "010", "label": "NYA",  "session_start": "09:30", "session_end": "12:00", "tp": 0.5, "sl": 6.0},
    {"id": "010", "label": "NYA",  "session_start": "09:30", "session_end": "12:00", "tp": 0.3, "sl": 8.0},
    {"id": "combined", "label": "NY", "session_start": "09:30", "session_end": "16:00", "tp": 0.3, "sl": 6.0},
    {"id": "combined", "label": "NY", "session_start": "09:30", "session_end": "16:00", "tp": 0.3, "sl": 10.0},
    {"id": "combined", "label": "NY", "session_start": "09:30", "session_end": "16:00", "tp": 0.5, "sl": 6.0},
]

RUNNER_MODES = [
    ("baseline", {}),
    ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
    ("trail_1.0x", {"mode": "trail", "trail_atr_mult": 1.0}),
    ("trail_1.5x", {"mode": "trail", "trail_atr_mult": 1.5}),
    ("trail_2.0x", {"mode": "trail", "trail_atr_mult": 2.0}),
    ("hold_day", {"mode": "hold_day"}),
    ("hold_session", {"mode": "hold_session"}),
]


def _build_cfg(run: dict) -> dict:
    if run["id"] == "combined":
        cfg = get_strategy_config("010")
    else:
        cfg = get_strategy_config(run["id"])
    cfg.update(
        {
            "session": run["label"],
            "session_start": run["session_start"],
            "session_end": run["session_end"],
            "tp_atr": run["tp"],
            "sl_atr": run["sl"],
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def _run_backtest(signals: pd.DataFrame) -> dict:
    if signals.empty:
        return {}
    bt = run_backtest(signals, {"point_value": POINT_VALUE, "topstep": {"enabled": True}})
    s = bt["summary"]
    executed = int(s.get("executed_trades", s.get("total_trades", 0)))
    total_pnl = float(s.get("total_pnl_points", 0)) * POINT_VALUE
    return {
        "total_return": float(s.get("total_return", 0)),
        "win_rate": float(s.get("win_rate", 0)),
        "executed_trades": executed,
        "account_failed": bool(s.get("account_failed", False)),
        "max_drawdown": float(s.get("max_drawdown", 0)),
        "total_pnl_dollars": total_pnl,
        "avg_per_trade": total_pnl / executed if executed else 0,
        "avg_per_week": total_pnl / 10 / 52,
        "avg_per_day": total_pnl / 10 / 252,
    }


def _all_combos():
    combos = []
    for run in BASE_RUNS:
        for mode_name, kwargs in RUNNER_MODES:
            combos.append((run, mode_name, kwargs))
    return combos


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--batch-id", type=int, required=True)
    parser.add_argument("--n-batches", type=int, required=True)
    parser.add_argument("--data-dir", type=str, default=str(_PROJECT_ROOT / "data"))
    parser.add_argument("--output-dir", type=str, required=True)
    args = parser.parse_args()

    data_dir = Path(args.data_dir)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    df = load_market_data(data_dir / "NQ_1min.parquet")
    df = df.copy()
    df["atr"] = _atr(df, 14)

    combos = _all_combos()
    my_combos = [c for i, c in enumerate(combos) if i % args.n_batches == args.batch_id]
    print(f"Batch {args.batch_id}/{args.n_batches}: {len(my_combos)} combos")

    for run, mode_name, kwargs in my_combos:
        cfg = _build_cfg(run)
        mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
        df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
        signals = generate_signals(df_s, cfg)
        if signals.empty:
            continue

        session_end_time = pd.Timestamp(cfg["session_end"]).time()
        day_end_time = pd.Timestamp("16:00").time()

        if mode_name == "baseline":
            runner_signals = signals.copy()
        else:
            kw = dict(kwargs)
            if kw.get("mode") == "hold_session":
                kw["session_end_time"] = session_end_time
            if kw.get("mode") == "hold_day":
                kw["day_end_time"] = day_end_time
            runner_signals = apply_runner_to_signals(df_s, signals, **kw)

        stats = _run_backtest(runner_signals)
        if not stats:
            continue

        record = {
            "base_id": run["id"],
            "session": cfg["session"],
            "tp": run["tp"],
            "sl": run["sl"],
            "mode": mode_name,
            **stats,
        }
        fname = f"runner_{run['id']}_{cfg['session']}_tp{run['tp']}_sl{run['sl']}_{mode_name}.json"
        (out_dir / fname).write_text(json.dumps(record, indent=2, default=str))
        print(f"  wrote {fname} weekly=${stats['avg_per_week']:.0f}")


if __name__ == "__main__":
    main()
