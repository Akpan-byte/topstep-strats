#!/usr/bin/env python3
"""Parallel TP/SL sweep for Paper-1 Casper Inverted FVG on NQ.

Tests the top Paper-1 edge under different take-profit / stop-loss ATR
multipliers, keeping win rate >= ~80% while hunting $5k/week average.
"""
from __future__ import annotations

import itertools
import multiprocessing as mp
import sys
from pathlib import Path

_PROJECT_ROOT = Path("/home/akpan/topstep-strats-paper2")
sys.path.insert(0, str(_PROJECT_ROOT))

import pandas as pd

from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import get_strategy_config, generate_signals

DATA_PATH = _PROJECT_ROOT / "data" / "NQ_1min.parquet"
POINT_VALUE = 20.0

TP_VALUES = [0.1, 0.2, 0.3, 0.5, 0.7, 1.0, 1.5, 2.0, 2.5, 3.0]
SL_VALUES = [2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0]

# Strategy IDs for Casper NQ.  Override their native session windows so we can
# test both halves and the full NY session.
BASE_CONFIGS = [
    {"id": "010", "label": "NYA", "session_start": "09:30", "session_end": "12:00"},
    {"id": "007", "label": "NYP", "session_start": "12:00", "session_end": "16:00"},
    {"id": "063", "label": "NYP2", "session_start": "12:00", "session_end": "16:00"},
    {"id": "combined", "label": "NY", "session_start": "09:30", "session_end": "16:00"},
]


def _build_cfg(base: dict, tp_atr: float, sl_atr: float) -> dict:
    if base["id"] == "combined":
        cfg = get_strategy_config("010")  # Casper NQ NYA as base blueprint/filter
    else:
        cfg = get_strategy_config(base["id"])
    cfg.update(
        {
            "session": base["label"],
            "session_start": base["session_start"],
            "session_end": base["session_end"],
            "tp_atr": tp_atr,
            "sl_atr": sl_atr,
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def _run_one(args) -> dict | None:
    base, tp_atr, sl_atr, df = args
    cfg = _build_cfg(base, tp_atr, sl_atr)
    try:
        mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
        df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
        signals = generate_signals(df_s, cfg)
        if signals.empty:
            return None
        bt = run_backtest(signals, {"point_value": POINT_VALUE, "topstep": {"enabled": True}})
        summary = bt["summary"]
        executed = int(summary.get("executed_trades", summary.get("total_trades", 0)))
        total_pnl = float(summary.get("total_pnl_points", 0)) * POINT_VALUE
        avg_per_trade = total_pnl / executed if executed else 0
        return {
            "base_id": base["id"],
            "session": cfg["session"],
            "filter": cfg.get("filter"),
            "tp_atr": tp_atr,
            "sl_atr": sl_atr,
            "total_return": float(summary.get("total_return", 0)),
            "win_rate": float(summary.get("win_rate", 0)),
            "trades": int(summary.get("total_trades", 0)),
            "executed_trades": executed,
            "account_failed": bool(summary.get("account_failed", False)),
            "max_drawdown": float(summary.get("max_drawdown", 0)),
            "total_pnl_dollars": total_pnl,
            "avg_per_trade": avg_per_trade,
            "avg_per_week": total_pnl / 10 / 52,
            "avg_per_day": total_pnl / 10 / 252,
        }
    except Exception as e:
        return {
            "base_id": base["id"],
            "session": cfg["session"],
            "filter": cfg.get("filter"),
            "tp_atr": tp_atr,
            "sl_atr": sl_atr,
            "error": str(e),
        }


def main():
    df = load_market_data(DATA_PATH)
    df = df.copy()

    tasks = [
        (base, tp, sl, df)
        for base in BASE_CONFIGS
        for tp in TP_VALUES
        for sl in SL_VALUES
    ]

    print(f"Running {len(tasks)} TP/SL combos on {mp.cpu_count()} cores...")
    with mp.Pool(processes=min(mp.cpu_count(), 16)) as pool:
        results = []
        for i, res in enumerate(pool.imap_unordered(_run_one, tasks), 1):
            if res and "error" not in res:
                results.append(res)
                status = "FAIL" if res["account_failed"] else "OK"
                print(
                    f"[{i}/{len(tasks)}] {res['base_id']:7s} {res['session']:4s} "
                    f"tp={res['tp_atr']:.1f} sl={res['sl_atr']:.1f} "
                    f"ret={res['total_return']:.3f} wr={res['win_rate']:.1%} "
                    f"trades={res['executed_trades']:4d} [{status}] "
                    f"weekly=${res['avg_per_week']:.0f}"
                )
            elif res and "error" in res:
                print(f"[{i}/{len(tasks)}] {res['base_id']} tp={res['tp_atr']} sl={res['sl_atr']} ERROR: {res['error']}")

    out = Path("/tmp/casper_paper1_tp_sl_sweep.csv")
    df_out = pd.DataFrame(results)
    df_out.to_csv(out, index=False)
    print(f"\nWrote {len(df_out)} results to {out}")

    print("\n=== TOP 15 BY WEEKLY INCOME ===")
    top = df_out.sort_values("avg_per_week", ascending=False).head(15)
    print(top[["base_id", "session", "filter", "tp_atr", "sl_atr", "total_return", "win_rate", "executed_trades", "account_failed", "avg_per_trade", "avg_per_week", "max_drawdown"]].to_string(index=False))

    print("\n=== TOP 15 BY TOTAL RETURN (win rate >= 80%) ===")
    filtered = df_out[(df_out["win_rate"] >= 0.80) & (~df_out["account_failed"])].sort_values("total_return", ascending=False).head(15)
    if not filtered.empty:
        print(filtered[["base_id", "session", "filter", "tp_atr", "sl_atr", "total_return", "win_rate", "executed_trades", "avg_per_trade", "avg_per_week", "max_drawdown"]].to_string(index=False))
    else:
        print("No combos with win rate >= 80% and alive account.")


if __name__ == "__main__":
    main()
