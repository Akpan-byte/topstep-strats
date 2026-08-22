#!/usr/bin/env python3
# CHANGE_SUMMARY
# 2026-08-20  coder
#   - Created scripts/focused_30min_sweep.py, a laptop-friendly (< 30 min)
#     focused sweep across the highest-probability Paper-1 and Paper-2
#     strategy variants identified in gh_results/deep_dive_top_performers.csv
#     and gh_results/paper2_focused_sweep.csv.
#   - Paper-1: top 8 real strategy IDs from deep_dive (casper) run across
#     NQ/ES and NY/London with a 5-point focused TP/SL grid.  Paper-1 signal
#     generation is ~3-4x slower than Paper-2, so the scope is kept tight to
#     finish well under 30 minutes on a 16-worker laptop.
#   - Paper-2: P2_001-P2_005 across NQ/ES and NY/London/Asian with a 4-point
#     focused TP/SL grid centred on the P2_001 winners.
#   - Runner modes: baseline, trail_0.5x, trail_1.0x.  hold_session is excluded
#     by default because paper2_focused_sweep.csv shows it produces huge
#     unrealized drawdowns and many account blowups; enable with --modes.
#   - Uses multiprocessing with instrument-by-instrument data loading (matches
#     sweep_paper1_comprehensive.py) and signal caching per
#     (paper, id, instrument, session, tp, sl) so runner modes reuse signals.
#   - Outputs a single CSV: gh_results/focused_30min_sweep.csv.
# WHY: The comprehensive Paper-1 sweep is estimated at ~5 hours on the laptop.
#      This script captures the highest-probability parameter neighbourhood in
#      well under 30 minutes, allowing rapid iteration without interfering with
#      the already-running remote sweep.

"""Focused 30-minute sweep across highest-probability Paper-1 and Paper-2 variants."""
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
    _atr as p1_atr,
    generate_signals as p1_generate_signals,
    get_strategy_config as p1_get_strategy_config,
)
from topstep_strats.strategies.paper2_matrix import (
    _atr as p2_atr,
    generate_signals as p2_generate_signals,
    get_strategy_config as p2_get_strategy_config,
)
from scripts.runner_exit_numba import apply_runner_to_signals

INSTRUMENTS = ["NQ", "ES"]
SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}
TICK_SIZES = {"NQ": 0.25, "ES": 0.25, "YM": 1.0}

# Top Paper-1 strategy IDs from deep_dive_top_performers.csv (real matrix IDs
# only, ranked by max total_return * win_rate).  Paper-1 signal generation is
# ~3-4x slower than Paper-2, so the ID list is kept tight to stay under 30 min
# on a 16-worker laptop.
PAPER1_FOCUS_IDS = [
    "061",  # top ranked overall
    "007",
    "011",
    "059",
    "009",
    "057",
    "010",
    "058",
]

# Sessions to sweep for Paper-1.  deep_dive_summary.md shows NY and London have
# the highest median scores; Asian is dropped here to save runtime while keeping
# the highest-probability neighbourhood.
PAPER1_SESSIONS = {
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

# Paper-2: focus on the first 5 IDs.  P2_001 is the only one with existing
# results, and it produced strong trail_0.5x / baseline outcomes on NQ/ES NY
# and London.  Running P2_001-P2_005 captures the immediate neighbourhood
# without exploding the runtime budget.
PAPER2_FOCUS_IDS = [f"P2_{i:03d}" for i in range(1, 6)]

# Focused TP/SL grid centred on the Casper TP/SL sweep winners.
# The deep_dive winners cluster around tp 0.3-0.5 and sl 6-10.
PAPER1_TP_SL_GRID = [
    (0.3, 6.0),
    (0.3, 10.0),
    (0.5, 6.0),
    (0.5, 10.0),
    (1.0, 10.0),
]

# Paper-2 focused grid around the P2_001 winners (tp 0.5/1.0 x sl 6/10).
PAPER2_TP_SL_GRID = [
    (0.5, 6.0),
    (0.5, 10.0),
    (1.0, 6.0),
    (1.0, 10.0),
]

# Realistic runner modes.  hold_session is excluded by default because
# paper2_focused_sweep.csv shows it generates huge max_drawdown_pct values
# (often > -80%) and many daily_limit_hits / account_blowups.
RUNNER_MODES = [
    ("baseline", {}),
    ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
    ("trail_1.0x", {"mode": "trail", "trail_atr_mult": 1.0}),
]

OUTPUT_COLUMNS = [
    "paper",
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


# ---------------------------------------------------------------------------
# Config builders
# ---------------------------------------------------------------------------
def _build_p1_cfg(sid: str, instrument: str, session: str, start: str, end: str, tp: float, sl: float) -> dict:
    cfg = p1_get_strategy_config(sid)
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


def _build_p2_cfg(sid: str, instrument: str, session: str, start: str, end: str, tp: float, sl: float) -> dict:
    cfg = p2_get_strategy_config(sid)
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


# ---------------------------------------------------------------------------
# Backtest helpers
# ---------------------------------------------------------------------------
def _hold_seconds(trades: pd.DataFrame) -> tuple[float, float]:
    """Return (avg_hold_seconds, median_hold_seconds) for executed trades."""
    if trades.empty or "exit_time" not in trades.columns:
        return 0.0, 0.0
    executed = trades[~trades.get("skipped", pd.Series(False, index=trades.index))]
    if executed.empty:
        return 0.0, 0.0
    deltas = pd.to_datetime(executed["exit_time"]) - pd.to_datetime(executed["entry_time"])
    hold = deltas.dt.total_seconds()
    return float(hold.mean()), float(hold.median())


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
# Multiprocessing workers (one instrument at a time)
# ---------------------------------------------------------------------------
_worker_instr: str = ""
_worker_df: pd.DataFrame | None = None
_worker_pv: float = 0.0


def _worker_init(instr: str, data_dir: str):
    """Bind the global instrument DataFrame inherited from the parent process."""
    global _worker_instr, _worker_pv
    _worker_instr = instr
    _worker_pv = POINT_VALUES[instr]
    if _worker_df is None:
        df = load_market_data(Path(data_dir) / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = p1_atr(df, 14)
        _set_worker_df(df)


def _set_worker_df(df: pd.DataFrame | None):
    global _worker_df
    _worker_df = df


def _signal_keys_for_instrument(instr: str, p1_ids: list[str], p2_ids: list[str]) -> list[tuple]:
    """All signal-cache keys for one instrument.

    A key is (paper, sid, instrument, session_name, start, end, tp, sl).
    """
    keys = []
    for sid in p1_ids:
        for sess, (start, end) in PAPER1_SESSIONS.items():
            for tp, sl in PAPER1_TP_SL_GRID:
                keys.append(("paper1", sid, instr, sess, start, end, tp, sl))
    for sid in p2_ids:
        for sess, (start, end) in SESSIONS.items():
            for tp, sl in PAPER2_TP_SL_GRID:
                keys.append(("paper2", sid, instr, sess, start, end, tp, sl))
    return keys


def _worker_run(key: tuple) -> list[dict]:
    """Generate signals for one key and run all requested runner modes."""
    paper, sid, instr, sess, start, end, tp, sl = key
    df = _worker_df
    if df is None:
        return []

    if paper == "paper1":
        cfg = _build_p1_cfg(sid, instr, sess, start, end, tp, sl)
        gen = p1_generate_signals
    else:
        cfg = _build_p2_cfg(sid, instr, sess, start, end, tp, sl)
        gen = p2_generate_signals

    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
    df_s.attrs = {}  # avoid cached array attr comparison issues
    signals = gen(df_s, cfg)

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
                "paper": paper,
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


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Run a focused 30-minute sweep of high-probability Paper-1/Paper-2 variants."
    )
    parser.add_argument("--workers", type=int, default=min(mp.cpu_count(), 16))
    parser.add_argument("--data-dir", type=str, default=str(_PROJECT_ROOT / "data"))
    parser.add_argument("--output-dir", type=str, default=str(_PROJECT_ROOT / "gh_results"))
    parser.add_argument("--output", type=str, default="focused_30min_sweep.csv")
    parser.add_argument("--flush-every", type=int, default=25)
    parser.add_argument("--instrument", type=str, default=None, choices=INSTRUMENTS + ["YM"])
    parser.add_argument(
        "--p1-ids",
        type=str,
        default=",".join(PAPER1_FOCUS_IDS),
        help="Comma-separated Paper-1 IDs to sweep",
    )
    parser.add_argument(
        "--p2-ids",
        type=str,
        default=",".join(PAPER2_FOCUS_IDS),
        help="Comma-separated Paper-2 IDs to sweep",
    )
    parser.add_argument(
        "--modes",
        type=str,
        default="baseline,trail_0.5x,trail_1.0x",
        help="Comma-separated runner modes",
    )
    args = parser.parse_args()

    data_dir = Path(args.data_dir)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / args.output

    instruments = [args.instrument] if args.instrument else INSTRUMENTS
    p1_ids = [s.strip() for s in args.p1_ids.split(",") if s.strip()]
    p2_ids = [s.strip() for s in args.p2_ids.split(",") if s.strip()]

    # Parse custom modes if requested.
    requested_modes = set(args.modes.split(","))
    global RUNNER_MODES
    available_modes = {
        "baseline": ("baseline", {}),
        "trail_0.5x": ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
        "trail_1.0x": ("trail_1.0x", {"mode": "trail", "trail_atr_mult": 1.0}),
        "hold_session": ("hold_session", {"mode": "hold_session"}),
    }
    RUNNER_MODES = [available_modes[m] for m in requested_modes if m in available_modes]
    if not RUNNER_MODES:
        raise ValueError(f"No valid modes in {args.modes!r}")

    total_keys = sum(
        len(_signal_keys_for_instrument(instr, p1_ids, p2_ids)) for instr in instruments
    )
    total_combos = total_keys * len(RUNNER_MODES)
    print(
        f"Focused sweep: {total_keys} signal keys x {len(RUNNER_MODES)} modes = "
        f"~{total_combos} combos across {len(instruments)} instrument(s) with {args.workers} workers"
    )

    all_records: list[dict] = []

    for instr in instruments:
        print(f"Loading {instr} data...")
        df = load_market_data(data_dir / f"{instr}_1min.parquet")
        df = df.copy()
        df["atr"] = p1_atr(df, 14)
        _set_worker_df(df)

        keys = _signal_keys_for_instrument(instr, p1_ids, p2_ids)
        n_keys = len(keys)
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
            for result in pool.imap_unordered(_worker_run, keys):
                all_records.extend(result)
                completed += 1
                if completed % args.flush_every == 0:
                    pd.DataFrame(all_records)[OUTPUT_COLUMNS].to_csv(out_path, index=False)
                    last = result[-1] if result else {}
                    print(
                        f"  [{instr}] {completed}/{n_keys} keys, {len(all_records)} total records, "
                        f"last={last.get('paper','')} {last.get('strategy_id','')} {last.get('session','')} "
                        f"{last.get('mode','')} weekly=${last.get('avg_per_week',0):.0f}"
                    )

        _set_worker_df(None)
        del df

    if all_records:
        df_out = pd.DataFrame(all_records)[OUTPUT_COLUMNS]
        df_out.to_csv(out_path, index=False)
        print(f"Wrote {len(df_out)} rows to {out_path}")
    else:
        print("No records produced")


if __name__ == "__main__":
    main()
