# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created scripts/sweep_paper1_comprehensive.py to run the full Paper-1
#     matrix sweep: 100 strategy IDs x NQ/ES/YM x Asian/London/NY sessions x
#     12 TP/SL pairs x 4 runner modes.
#   - Signals are cached per (strategy_id, instrument, session, tp, sl) and
#     reused across runner modes to avoid redundant generation.
#   - Supports --batch-id/--n-batches partitioning for GitHub Actions; writes
#     a single CSV (or per-batch partition when n_batches > 1).
# WHY: The Paper-1 comprehensive sweep needs a dedicated, reusable runner that
#      produces the exact CSV schema expected by the aggregation/ranking step.

#!/usr/bin/env python3
"""Comprehensive Paper-1 sweep runner.

Full grid: 100 strategy IDs x NQ/ES/YM x Asian/London/NY x 12 (tp,sl) pairs x
4 runner modes.  Signals are cached per (id, instrument, session, tp, sl) and
reused across modes.  Topstep reset-on-failure is enabled so blown accounts
model buying a new combine.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

import numpy as np
import pandas as pd

from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import (
    generate_signals,
    get_strategy_config,
    list_strategy_ids,
    _atr,
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

TP_SL_GRID = [
    (0.3, 4.0),
    (0.3, 6.0),
    (0.3, 8.0),
    (0.3, 10.0),
    (0.5, 4.0),
    (0.5, 6.0),
    (0.5, 8.0),
    (0.5, 10.0),
    (0.7, 6.0),
    (1.0, 6.0),
    (1.5, 6.0),
    (2.0, 6.0),
]

RUNNER_MODES = [
    ("baseline", {}),
    ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
    ("trail_1.0x", {"mode": "trail", "trail_atr_mult": 1.0}),
    ("hold_session", {"mode": "hold_session"}),
]

OUTPUT_COLUMNS = [
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


def _all_combos() -> list[tuple]:
    combos = []
    for sid in list_strategy_ids():
        for instr in INSTRUMENTS:
            for sess, (start, end) in SESSIONS.items():
                for tp, sl in TP_SL_GRID:
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
    """Return average and median hold duration in seconds for executed trades."""
    if trades.empty or "exit_time" not in trades.columns:
        return 0.0, 0.0
    deltas = (pd.to_datetime(trades["exit_time"]) - pd.to_datetime(trades["entry_time"])).dt.total_seconds()
    if deltas.empty:
        return 0.0, 0.0
    return float(deltas.mean()), float(deltas.median())


def _run_backtest(signals: pd.DataFrame, point_value: float) -> dict:
    if signals.empty:
        return {}
    bt = run_backtest(
        signals,
        {"point_value": point_value, "topstep": {"enabled": True, "reset_on_failure": True}},
    )
    s = bt["summary"]
    executed = int(s.get("executed_trades", 0))
    total_pnl = float(s.get("cumulative_pnl_dollars", 0))
    executed_trades = bt["trades"][~bt["trades"]["skipped"]] if "skipped" in bt["trades"].columns else bt["trades"]
    avg_hold, median_hold = _hold_seconds(executed_trades)
    return {
        "win_rate": float(s.get("win_rate", 0)),
        "executed_trades": executed,
        "account_blowups": int(s.get("account_blowups", 0)),
        "daily_limit_hits": int(s.get("daily_limit_hits", 0)),
        "max_drawdown_pct": float(s.get("max_drawdown_pct", 0)),
        "total_pnl_dollars": total_pnl,
        "avg_per_trade": total_pnl / executed if executed else 0.0,
        "avg_per_week": total_pnl / 10 / 52,
        "avg_per_day": total_pnl / 10 / 252,
        "avg_hold_seconds": avg_hold,
        "median_hold_seconds": median_hold,
    }


def _output_path(out_dir: Path, batch_id: int, n_batches: int) -> Path:
    if n_batches <= 1:
        return out_dir / "paper1_comprehensive_sweep.csv"
    return out_dir / f"paper1_comprehensive_sweep_batch_{batch_id}_of_{n_batches}.csv"


def main():
    parser = argparse.ArgumentParser(description="Run the comprehensive Paper-1 sweep.")
    parser.add_argument("--batch-id", type=int, default=0)
    parser.add_argument("--n-batches", type=int, default=1)
    parser.add_argument("--data-dir", type=str, default=str(_PROJECT_ROOT / "data"))
    parser.add_argument("--output-dir", type=str, default=str(_PROJECT_ROOT / "gh_results"))
    args = parser.parse_args()

    if args.n_batches < 1:
        raise ValueError("--n-batches must be >= 1")
    if not (0 <= args.batch_id < args.n_batches):
        raise ValueError("--batch-id must be in [0, --n-batches)")

    data_dir = Path(args.data_dir)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Load all instruments once and pre-compute ATR for the runner.
    dfs: dict[str, pd.DataFrame] = {}
    for instr in INSTRUMENTS:
        df = load_market_data(data_dir / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = _atr(df, 14)
        dfs[instr] = df

    combos = _all_combos()
    my_combos = [c for i, c in enumerate(combos) if i % args.n_batches == args.batch_id]
    print(f"Batch {args.batch_id}/{args.n_batches}: {len(my_combos)} combos")

    # Cache only the most recent signal set.  The combo order keeps all modes for
    # a given (id, instrument, session, tp, sl) together, so signals are reused
    # across the 4 runner modes without accumulating every key in memory.
    last_key: tuple | None = None
    last_signals: pd.DataFrame | None = None
    records: list[dict] = []

    for idx, (sid, instr, sess, start, end, tp, sl, mode_name, kwargs) in enumerate(my_combos, start=1):
        cache_key = (sid, instr, sess, tp, sl)
        if cache_key != last_key:
            cfg = _build_cfg(sid, instr, sess, start, end, tp, sl)
            df = dfs[instr]
            mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
            df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
            last_signals = generate_signals(df_s, cfg)
            last_key = cache_key

        signals = last_signals
        if signals is None or signals.empty:
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
        records.append(record)
        print(
            f"[{idx}/{len(my_combos)}] {sid} {instr} {sess} tp={tp} sl={sl} {mode_name} "
            f"wr={stats['win_rate']:.1%} trades={stats['executed_trades']:4d} "
            f"weekly=${stats['avg_per_week']:.0f} blowups={stats['account_blowups']:3d} "
            f"hold={stats['avg_hold_seconds']:.0f}s"
        )

    if records:
        df_out = pd.DataFrame(records)[OUTPUT_COLUMNS]
        out_path = _output_path(out_dir, args.batch_id, args.n_batches)
        df_out.to_csv(out_path, index=False)
        print(f"Wrote {len(records)} rows to {out_path}")
    else:
        print("No records produced")


if __name__ == "__main__":
    main()
