# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created scripts/sweep_paper1_comprehensive.py to run the full Paper-1
#     matrix sweep: 100 strategy IDs x NQ/ES/YM x Asian/London/NY sessions x
#     12 TP/SL pairs x 4 runner modes.
#   - Signals are cached per (strategy_id, instrument, session, tp, sl) and
#     reused across runner modes to avoid redundant generation.
#   - Supports --batch-id/--n-batches partitioning for GitHub Actions; writes
#     a single CSV (or per-batch partition when n_batches > 1).
# 2026-08-19  kilo
#   - Replaced the single-threaded combo loop with multiprocessing.Pool.
#   - Added --workers CLI argument and periodic CSV flushes.
# 2026-08-19  kilo
#   - Switched to instrument-by-instrument processing.  The parent loads one
#     instrument, forks a pool of workers, and processes every strategy/session/
#     tp/sl key for that instrument before moving to the next instrument.  This
#     keeps the per-worker memory footprint to one instrument (~400 MB) instead
#     of all three (~1.4 GB), so 16 workers can run without swapping on a 16 GB+
#     machine.
#   - Workers inherit the already-loaded DataFrame via copy-on-write; each
#     worker only materializes its own signal table.
#   - Reduced default --flush-every to 25 so progress is visible sooner.
# WHY: 14 workers loading all three instruments each caused the laptop to swap
#      and stall.  Processing one instrument at a time with all 16 cores is both
#      faster and memory-safe.

#!/usr/bin/env python3
"""Comprehensive Paper-1 sweep runner (multiprocessing, instrument-by-instrument)."""
from __future__ import annotations

import argparse
import multiprocessing as mp
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

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


def _signal_keys_for_instrument(instr: str) -> list[tuple]:
    """All (id, instrument, session, start, end, tp, sl) keys for one instrument."""
    keys = []
    for sid in list_strategy_ids():
        for sess, (start, end) in SESSIONS.items():
            for tp, sl in TP_SL_GRID:
                keys.append((sid, instr, sess, start, end, tp, sl))
    return keys


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


# ---------------------------------------------------------------------------
# Multiprocessing worker helpers (one instrument at a time)
# ---------------------------------------------------------------------------

_worker_instr: str = ""
_worker_df: pd.DataFrame | None = None
_worker_pv: float = 0.0


def _worker_init(instr: str, data_dir: str):
    """Load a single instrument in the parent; workers inherit it via fork/COW."""
    global _worker_instr, _worker_df, _worker_pv
    _worker_instr = instr
    _worker_pv = POINT_VALUES[instr]
    # Data is loaded by the parent and passed in; this function just binds it.
    # If called directly (rare), load from disk as fallback.
    if _worker_df is None:
        df = load_market_data(Path(data_dir) / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = _atr(df, 14)
        _worker_df = df


def _worker_set_df(df: pd.DataFrame):
    """Called in the parent before forking so workers inherit the DataFrame."""
    global _worker_df
    _worker_df = df


def _worker_run(key: tuple) -> list[dict]:
    """Generate signals for one key and run all runner modes."""
    sid, instr, sess, start, end, tp, sl = key
    cfg = _build_cfg(sid, instr, sess, start, end, tp, sl)
    df = _worker_df
    if df is None:
        return []
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
    signals = generate_signals(df_s, cfg)

    if signals is None or signals.empty:
        return []

    records: list[dict] = []
    for mode_name, kwargs in RUNNER_MODES:
        if mode_name == "baseline":
            runner_signals = signals.copy()
        else:
            kw = dict(kwargs)
            if kw.get("mode") == "hold_session":
                kw["session_end_time"] = pd.Timestamp(end).time()
            runner_signals = apply_runner_to_signals(df, signals, **kw)

        stats = _run_backtest(runner_signals, _worker_pv)
        if not stats:
            continue

        records.append(
            {
                "strategy_id": sid,
                "instrument": instr,
                "session": sess,
                "tp": tp,
                "sl": sl,
                "mode": mode_name,
                **stats,
            }
        )
    return records


def _output_path(out_dir: Path, batch_id: int, n_batches: int) -> Path:
    if n_batches <= 1:
        return out_dir / "paper1_comprehensive_sweep.csv"
    return out_dir / f"paper1_comprehensive_sweep_batch_{batch_id}_of_{n_batches}.csv"


def main():
    parser = argparse.ArgumentParser(description="Run the comprehensive Paper-1 sweep.")
    parser.add_argument("--batch-id", type=int, default=0)
    parser.add_argument("--n-batches", type=int, default=1)
    parser.add_argument("--workers", type=int, default=min(mp.cpu_count(), 16))
    parser.add_argument("--data-dir", type=str, default=str(_PROJECT_ROOT / "data"))
    parser.add_argument("--output-dir", type=str, default=str(_PROJECT_ROOT / "gh_results"))
    parser.add_argument("--flush-every", type=int, default=25, help="Write CSV every N keys")
    parser.add_argument("--instrument", type=str, default=None, help="Run a single instrument (NQ/ES/YM)")
    args = parser.parse_args()

    if args.n_batches < 1:
        raise ValueError("--n-batches must be >= 1")
    if not (0 <= args.batch_id < args.n_batches):
        raise ValueError("--batch-id must be in [0, --n-batches)")

    data_dir = Path(args.data_dir)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    instruments = [args.instrument] if args.instrument else INSTRUMENTS
    if args.instrument and args.instrument not in INSTRUMENTS:
        raise ValueError(f"--instrument must be one of {INSTRUMENTS}")

    out_path = _output_path(out_dir, args.batch_id, args.n_batches)
    all_records: list[dict] = []

    for instr in instruments:
        print(f"Loading {instr} data...")
        df = load_market_data(data_dir / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = _atr(df, 14)
        _worker_set_df(df)

        keys = _signal_keys_for_instrument(instr)
        my_keys = [k for i, k in enumerate(keys) if i % args.n_batches == args.batch_id]
        n_keys = len(my_keys)
        n_combos = n_keys * len(RUNNER_MODES)
        print(
            f"Instrument {instr}: {n_keys} signal keys -> ~{n_combos} combos using {args.workers} workers"
        )

        completed = 0
        with mp.Pool(
            processes=args.workers,
            initializer=_worker_init,
            initargs=(instr, str(data_dir)),
        ) as pool:
            for result in pool.imap_unordered(_worker_run, my_keys):
                all_records.extend(result)
                completed += 1
                if completed % args.flush_every == 0:
                    pd.DataFrame(all_records)[OUTPUT_COLUMNS].to_csv(out_path, index=False)
                    last = result[-1] if result else {}
                    print(
                        f"  [{instr}] {completed}/{n_keys} keys, {len(all_records)} total records, "
                        f"last={last.get('strategy_id','')} {last.get('session','')} {last.get('mode','')} "
                        f"weekly=${last.get('avg_per_week',0):.0f}"
                    )

        # Force free the instrument DataFrame before loading the next one.
        _worker_set_df(None)
        del df

    if all_records:
        df_out = pd.DataFrame(all_records)[OUTPUT_COLUMNS]
        df_out.to_csv(out_path, index=False)
        print(f"Wrote {len(all_records)} rows to {out_path}")
    else:
        print("No records produced")


if __name__ == "__main__":
    main()
