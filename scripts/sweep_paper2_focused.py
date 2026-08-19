#!/usr/bin/env python3
"""Focused Paper-2 sweep batch worker for GitHub Actions.

Runs the Paper-2 matrix (P2_001-P2_108) across NQ/ES and Asian/London/NY
sessions under a focused TP/SL grid and a small set of runner exit modes.
Each worker handles combos where ``index % n_batches == batch_id``.
Signals are cached per (id, instrument, session, tp, sl) and reused across
runner modes.  Topstep reset-on-failure is enabled so blown accounts model
buying a new combine.
"""
# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created scripts/sweep_paper2_focused.py to run a focused Paper-2 sweep
#     across P2_001-P2_108, NQ/ES, Asian/London/NY, four TP/SL pairs, and the
#     baseline/trail_0.5x/hold_session runner modes.
#   - Signals are cached per (id, instrument, session, tp, sl) and reused
#     across runner modes.
#   - Outputs a single CSV gh_results/paper2_focused_sweep.csv with the
#     canonical summary columns plus average/median hold durations.
#   - Supports --batch-id/--n-batches partitioning and --ids-file for testing.
# WHY: Provides a compact, parallelizable Paper-2 evaluation script that
#      follows the same conventions as the Paper-1 broad batch runner.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

import pandas as pd

from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper2_matrix import (
    get_strategy_config,
    generate_signals,
    _atr,
    list_strategy_ids,
)
from scripts.runner_exit_numba import apply_runner_to_signals

INSTRUMENTS = ["NQ", "ES", "YM"]
SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}
TICK_SIZES = {"NQ": 0.25, "ES": 0.25, "YM": 1.0}

TP_SL_SETTINGS = [(0.5, 6.0), (0.5, 10.0), (1.0, 6.0), (1.0, 10.0)]

RUNNER_MODES = [
    ("baseline", {}),
    ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
    ("hold_session", {"mode": "hold_session"}),
]


def _available_instruments(data_dir: Path) -> list[str]:
    """Return instruments whose parquet files exist in ``data_dir``."""
    available = []
    for instr in INSTRUMENTS:
        path = data_dir / f"{instr}_1min.parquet"
        if path.exists():
            available.append(instr)
    return available


def _all_combos(ids: list[str], instruments: list[str]):
    combos = []
    for sid in ids:
        for instr in instruments:
            for sess, (start, end) in SESSIONS.items():
                for tp, sl in TP_SL_SETTINGS:
                    for mode_name, kwargs in RUNNER_MODES:
                        combos.append((sid, instr, sess, start, end, tp, sl, mode_name, kwargs))
    return combos


def _build_cfg(sid: str, instrument: str, session: str, start: str, end: str, tp: float, sl: float) -> dict:
    cfg = get_strategy_config(sid)
    cfg.update(
        {
            "instrument": instrument,
            "tick_size": TICK_SIZES[instrument],
            "point_value": POINT_VALUES[instrument],
            "session": session,
            "session_start": start,
            "session_end": end,
            "tp_atr": tp,
            "sl_atr": sl,
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def _hold_seconds(trades: pd.DataFrame) -> tuple[float, float]:
    """Return (avg_hold_seconds, median_hold_seconds) for executed trades."""
    executed = trades[~trades.get("skipped", pd.Series(False, index=trades.index))]
    if executed.empty:
        return 0.0, 0.0
    hold = (pd.to_datetime(executed["exit_time"]) - pd.to_datetime(executed["entry_time"])).dt.total_seconds()
    return float(hold.mean()), float(hold.median())


def _run_backtest(runner_signals: pd.DataFrame, point_value: float) -> dict:
    if runner_signals.empty:
        return {}
    bt = run_backtest(
        runner_signals,
        {"point_value": point_value, "topstep": {"enabled": True, "reset_on_failure": True}},
    )
    s = bt["summary"]
    executed = int(s.get("executed_trades", 0))
    total_pnl = float(s.get("cumulative_pnl_dollars", 0))
    avg_hold, median_hold = _hold_seconds(bt["trades"])
    return {
        "win_rate": float(s.get("win_rate", 0)),
        "executed_trades": executed,
        "account_blowups": int(s.get("account_blowups", 0)),
        "daily_limit_hits": int(s.get("daily_limit_hits", 0)),
        "max_drawdown_pct": float(s.get("max_drawdown_pct", 0)),
        "total_pnl_dollars": total_pnl,
        "avg_per_trade": total_pnl / executed if executed else 0,
        "avg_per_week": total_pnl / 10 / 52,
        "avg_per_day": total_pnl / 10 / 252,
        "avg_hold_seconds": avg_hold,
        "median_hold_seconds": median_hold,
    }


def _read_ids_file(path: Path) -> list[str]:
    raw = path.read_text().splitlines()
    return [line.strip() for line in raw if line.strip()]


def main():
    parser = argparse.ArgumentParser(description="Focused Paper-2 sweep")
    parser.add_argument("--batch-id", type=int, default=0)
    parser.add_argument("--n-batches", type=int, default=1)
    parser.add_argument("--ids-file", type=str, default=None)
    parser.add_argument("--data-dir", type=str, default=str(_PROJECT_ROOT / "data"))
    parser.add_argument("--output-dir", type=str, default=str(_PROJECT_ROOT / "gh_results"))
    args = parser.parse_args()

    data_dir = Path(args.data_dir)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    instruments = _available_instruments(data_dir)
    if not instruments:
        raise FileNotFoundError(f"No instrument parquet files found in {data_dir}")

    if args.ids_file:
        ids = _read_ids_file(Path(args.ids_file))
    else:
        ids = list_strategy_ids()

    # Load all instruments once and pre-compute ATR for the runner.
    dfs = {}
    for instr in instruments:
        df = load_market_data(data_dir / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = _atr(df, 14)
        dfs[instr] = df

    combos = _all_combos(ids, instruments)
    my_combos = [c for i, c in enumerate(combos) if i % args.n_batches == args.batch_id]
    print(f"Batch {args.batch_id}/{args.n_batches}: {len(my_combos)} combos  instruments={instruments}")

    # Cache signals per (id, instrument, session, tp, sl).
    signal_cache: dict[tuple, pd.DataFrame] = {}
    rows: list[dict] = []

    for sid, instr, sess, start, end, tp, sl, mode_name, kwargs in my_combos:
        cache_key = (sid, instr, sess, tp, sl)
        signals = signal_cache.get(cache_key)
        if signals is None:
            cfg = _build_cfg(sid, instr, sess, start, end, tp, sl)
            df = dfs[instr]
            mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
            df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
            # Drop cached Numba/array attrs so pandas concat inside the strategy
            # helpers does not try to compare numpy arrays (pandas __finalize__).
            df_s.attrs = {}
            signals = generate_signals(df_s, cfg)
            signal_cache[cache_key] = signals

        if signals.empty:
            continue

        if mode_name == "baseline":
            runner_signals = signals.copy()
        else:
            kw = dict(kwargs)
            if kw.get("mode") == "hold_session":
                kw["session_end_time"] = pd.Timestamp(end).time()
            runner_signals = apply_runner_to_signals(dfs[instr], signals, **kw)

        stats = _run_backtest(runner_signals, POINT_VALUES[instr])
        if not stats:
            continue

        record = {
            "strategy_id": sid,
            "instrument": instr,
            "session": sess,
            "tp": tp,
            "sl": sl,
            "mode": mode_name,
            **stats,
        }
        rows.append(record)
        print(
            f"  {sid} {instr} {sess} tp={tp} sl={sl} {mode_name:14s} "
            f"wr={stats['win_rate']:.1%} trades={stats['executed_trades']:4d} "
            f"weekly=${stats['avg_per_week']:.0f} blowups={stats['account_blowups']:3d} "
            f"hold={stats['avg_hold_seconds']:.0f}s"
        )

    out_csv = out_dir / "paper2_focused_sweep.csv"
    if rows:
        df_out = pd.DataFrame(rows)
        cols = [
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
        df_out = df_out[cols]
        df_out.to_csv(out_csv, index=False)
        print(f"Wrote {len(df_out)} rows to {out_csv}")
    else:
        print("No rows produced")


if __name__ == "__main__":
    main()
