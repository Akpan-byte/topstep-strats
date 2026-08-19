#!/usr/bin/env python3
"""Broad runner sweep batch worker for GitHub Actions.

Full grid: top Paper-1 IDs x NQ/ES/YM x Asian/London/NY x (tp,sl) x runner modes.
Each worker handles combos where ``index % n_batches == batch_id``.
Signals are cached per (id, instrument, session, tp, sl) and reused across
runner modes.  Topstep reset-on-failure is enabled so blown accounts model
buying a new combine.
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
from scripts.runner_exit_numba import apply_runner_to_signals

# Top Paper-1 performers from the Topstep-ranked sweep (Casper dominates, with
# Velez and Rosato also appearing).  These are the IDs most likely to reach the
# user's $5k/week target while keeping win rate above ~80%.
TOP_IDS = [
    "061", "007", "011", "059", "009", "057", "010", "062", "058", "006",
    "017",  # Velez 20/200 Elephant Bar
    "073",  # Rosato S/D Absorption
]

INSTRUMENTS = ["NQ", "ES", "YM"]
SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}
TICK_SIZES = {"NQ": 0.25, "ES": 0.25, "YM": 1.0}

TP_SL_SETTINGS = [(0.5, 6.0), (0.5, 10.0), (0.3, 6.0)]

RUNNER_MODES = [
    ("baseline", {}),
    ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
    ("trail_1.0x", {"mode": "trail", "trail_atr_mult": 1.0}),
    ("hold_session", {"mode": "hold_session"}),
]


def _all_combos():
    combos = []
    for sid in TOP_IDS:
        for instr in INSTRUMENTS:
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
    return {
        "total_return_pct": float(s.get("total_return_pct", 0)),
        "win_rate": float(s.get("win_rate", 0)),
        "executed_trades": executed,
        "account_failed": bool(s.get("account_failed", False)),
        "account_blowups": int(s.get("account_blowups", 0)),
        "daily_limit_hits": int(s.get("daily_limit_hits", 0)),
        "max_drawdown_pct": float(s.get("max_drawdown_pct", 0)),
        "avg_per_trade": total_pnl / executed if executed else 0,
        "avg_per_week": total_pnl / 10 / 52,
        "avg_per_day": total_pnl / 10 / 252,
        "total_pnl_dollars": total_pnl,
    }


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

    # Load all instruments once.
    dfs = {}
    for instr in INSTRUMENTS:
        df = load_market_data(data_dir / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = _atr(df, 14)
        dfs[instr] = df

    combos = _all_combos()
    my_combos = [c for i, c in enumerate(combos) if i % args.n_batches == args.batch_id]
    print(f"Batch {args.batch_id}/{args.n_batches}: {len(my_combos)} combos")

    # Cache signals per (id, instrument, session, tp, sl).
    signal_cache: dict[tuple, pd.DataFrame] = {}

    for sid, instr, sess, start, end, tp, sl, mode_name, kwargs in my_combos:
        cache_key = (sid, instr, sess, tp, sl)
        signals = signal_cache.get(cache_key)
        if signals is None:
            cfg = _build_cfg(sid, instr, sess, start, end, tp, sl)
            df = dfs[instr]
            mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
            df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
            signals = generate_signals(df_s, cfg)
            if signals.empty:
                signal_cache[cache_key] = signals
            else:
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
            "paper": "paper1",
            "strategy_id": sid,
            "instrument": instr,
            "session": sess,
            "tp": tp,
            "sl": sl,
            "mode": mode_name,
            **stats,
        }
        fname = f"runner_{sid}_{instr}_{sess}_tp{tp}_sl{sl}_{mode_name}.json"
        (out_dir / fname).write_text(json.dumps(record, indent=2, default=str))
        print(
            f"  {fname} wr={stats['win_rate']:.1%} trades={stats['executed_trades']:4d} "
            f"weekly=${stats['avg_per_week']:.0f} blowups={stats['account_blowups']:3d}"
        )


if __name__ == "__main__":
    main()
