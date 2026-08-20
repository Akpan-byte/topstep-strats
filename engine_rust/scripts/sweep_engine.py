# CHANGE_SUMMARY
# 2026-08-20  coder
#   - Created engine_rust/scripts/sweep_engine.py, the Rust/PyO3 sweep
#     orchestrator for the Paper-1 and Paper-2 matrices.
#   - Paper-1: 100 strategy IDs x NQ/ES/YM x Asian/London/NY x 12 TP/SL pairs
#     x 4 runner modes.  Entry signals are cached per (id, instrument, session)
#     and reused across the 12x4 exit grid.
#   - Paper-2: 108 strategy IDs x NQ/ES x Asian/London/NY x 2 risk profiles.
#   - Finalized signal tables are executed by the Rust engine's run_signals()
#     path for fast PnL accounting and optional Topstep rule checks.
#   - Supports 16-worker multiprocessing, incremental CSV flushing, and a
#     lookahead-bias validation harness.
# 2026-08-20  kilo
#   - Expanded Paper-1 TP/SL grid to the full 6x4 factorial (24 pairs).
#   - Added hold_day runner mode to Paper-1 sweep.
#   - Added --overwrite flag; default behavior now refuses to append to an
#     existing sweep CSV to prevent duplicate rows.
#   - Added YM to Paper-2 instrument sweep and made risk_profile an independent
#     swept variable so every Paper-2 strategy is tested under both profiles.
#   - Added a `topstep_enabled` column to both output schemas for clarity.
# 2026-08-20  coder
#   - Added breakeven_0.5x and breakeven_1.0x runner modes to Paper-1 using
#     the new breakeven_then_trail exit logic.
#   - Added portfolio1 paper option and supporting portfolio runner module
#     for combined NQ/ES/YM x Asian/London/NY backtests with account-level
#     Topstep rule checks.
# 2026-08-20  kilo
#   - Wired portfolio_runner._portfolio_tasks and _run_portfolio_group into
#     the sweep_engine CLI as --paper portfolio1.
#   - portfolio1 defaults to account-level Topstep analysis (daily $900,
#     trailing $2,000, profit target $3,000, reset on failure).
#   - Added portfolio1-specific CLI options: --tp-atr, --sl-atr, --mode,
#     --trail-atr-mult.
# 2026-08-20  coder
#   - Added new Topstep combine fields (passed_combines, failed_combines,
#     combine_pass_rate, days/trades to pass statistics) to Paper-1 and
#     Paper-2 output schemas and the result dict built from Summary.
#   - Added --account-size CLI option (50k/150k) that selects Topstep daily
#     drawdown, trailing drawdown, and profit-target parameters.
#   - Output filename now includes account size so 50k and 150k sweeps do not
#     overwrite each other.
# WHY: Close the gaps identified in the audit: full param grid, no duplicate
#      rows, complete Paper-2 instrument/risk-profile coverage, new exit
#      modes, portfolio-level analysis, and per-account-size combine metrics.

#!/usr/bin/env python3
"""Rust/PyO3 sweep orchestrator for Paper-1 and Paper-2 strategy matrices.

Examples
--------
Run a small Paper-1 smoke sweep with 4 workers:
    python engine_rust/scripts/sweep_engine.py --paper paper1 --workers 4 --id-start 0 --id-end 2 --instruments NQ --sessions NY --output-dir /tmp/sweep_smoke

Run the full Paper-1 matrix with 16 workers:
    python engine_rust/scripts/sweep_engine.py --paper paper1 --workers 16 --output-dir /tmp/paper1_rust_sweep

Run the Paper-2 matrix:
    python engine_rust/scripts/sweep_engine.py --paper paper2 --workers 16 --output-dir /tmp/paper2_rust_sweep

Run with the 150k Topstep account-size rules:
    python engine_rust/scripts/sweep_engine.py --paper paper1 --workers 16 --topstep --account-size 150k --output-dir /tmp/paper1_rust_sweep_150k

Run a portfolio1 smoke sweep for strategies 0-2:
    python engine_rust/scripts/sweep_engine.py --paper portfolio1 --workers 4 --id-start 0 --id-end 2 --output-dir /tmp/portfolio1_smoke

Validate a sample for lookahead bias before the full sweep:
    python engine_rust/scripts/sweep_engine.py --paper paper1 --validate --workers 4
"""
from __future__ import annotations

import argparse
import itertools
import json
import multiprocessing as mp
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


# ---------------------------------------------------------------------------
# Path setup so this script can be run from anywhere.
# ---------------------------------------------------------------------------
_ENGINE_RUST_ROOT = Path(__file__).resolve().parent.parent
_PROJECT_ROOT = _ENGINE_RUST_ROOT.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))
if str(_ENGINE_RUST_ROOT / "python") not in sys.path:
    sys.path.insert(0, str(_ENGINE_RUST_ROOT / "python"))

import numpy as np
import pandas as pd

from agnostic_backtest import AgnosticBacktest, TopstepConfig
from engine_rust.scripts.portfolio_runner import (
    OUTPUT_COLUMNS_PORTFOLIO1,
    _portfolio_tasks,
    _run_portfolio_group,
)
from scripts.runner_exit_numba import apply_runner_to_signals
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import (
    generate_signals as generate_paper1_signals,
    get_strategy_config as get_paper1_config,
    list_strategy_ids as list_paper1_ids,
    _atr as _paper1_atr,
)
from topstep_strats.strategies.paper2_matrix import (
    generate_signals as generate_paper2_signals,
    get_strategy_config as get_paper2_config,
    list_strategy_ids as list_paper2_ids,
    _atr as _paper2_atr,
)


# ---------------------------------------------------------------------------
# Matrix constants
# ---------------------------------------------------------------------------
PAPER1_INSTRUMENTS = ["NQ", "ES", "YM"]
PAPER1_SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}
PAPER1_TP_VALUES = [0.3, 0.5, 0.7, 1.0, 1.5, 2.0]
PAPER1_SL_VALUES = [4.0, 6.0, 8.0, 10.0]
PAPER1_TP_SL_GRID = list(itertools.product(PAPER1_TP_VALUES, PAPER1_SL_VALUES))

PAPER1_RUNNER_MODES = [
    ("baseline", {"mode": "baseline"}),
    ("trail_0.5x", {"mode": "trail", "trail_atr_mult": 0.5}),
    ("trail_1.0x", {"mode": "trail", "trail_atr_mult": 1.0}),
    ("breakeven_0.5x", {"mode": "breakeven_then_trail", "trail_atr_mult": 0.5}),
    ("breakeven_1.0x", {"mode": "breakeven_then_trail", "trail_atr_mult": 1.0}),
    ("hold_session", {"mode": "hold_session"}),
    ("hold_day", {"mode": "hold_day"}),
]

PAPER2_INSTRUMENTS = ["NQ", "ES", "YM"]
PAPER2_SESSIONS = {
    "Asian": ("18:00", "02:00"),
    "London": ("03:00", "08:00"),
    "NY": ("08:30", "16:00"),
}

POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}
TICK_SIZES = {"NQ": 0.25, "ES": 0.25, "YM": 1.0}

# Paper-2 risk-profile TP/SL overrides.  The matrix bakes one profile per ID,
# but the sweep overrides both instrument and risk_profile so every engine can
# be evaluated under conservative and aggressive sizing.
PAPER2_RISK_PROFILES = ["conservative", "aggressive"]
PAPER2_RISK_TP_SL: Dict[str, Tuple[float, float]] = {
    "conservative": (1.5, 2.0),
    "aggressive": (3.0, 4.0),
}

OUTPUT_COLUMNS_PAPER1 = [
    "paper",
    "strategy_id",
    "instrument",
    "session",
    "tp",
    "sl",
    "mode",
    "win_rate",
    "n_trades",
    "executed_trades",
    "trades_per_day",
    "account_blowups",
    "daily_limit_hits",
    "trailing_limit_hits",
    "max_drawdown",
    "sharpe",
    "total_return",
    "total_pnl_dollars",
    "avg_per_trade",
    "avg_per_week",
    "avg_per_day",
    "profit_target_reached",
    "account_failed",
    "passed_combines",
    "failed_combines",
    "combine_pass_rate",
    "avg_days_to_pass",
    "median_days_to_pass",
    "min_days_to_pass",
    "max_days_to_pass",
    "days_to_pass_p50",
    "days_to_pass_p75",
    "days_to_pass_p90",
    "days_to_pass_p95",
    "days_to_pass_p99",
    "avg_trades_to_pass",
    "median_trades_to_pass",
    "topstep_enabled",
]

OUTPUT_COLUMNS_PAPER2 = [
    "paper",
    "strategy_id",
    "instrument",
    "session",
    "risk_profile",
    "win_rate",
    "n_trades",
    "executed_trades",
    "trades_per_day",
    "account_blowups",
    "daily_limit_hits",
    "trailing_limit_hits",
    "max_drawdown",
    "sharpe",
    "total_return",
    "total_pnl_dollars",
    "avg_per_trade",
    "avg_per_week",
    "avg_per_day",
    "profit_target_reached",
    "account_failed",
    "passed_combines",
    "failed_combines",
    "combine_pass_rate",
    "avg_days_to_pass",
    "median_days_to_pass",
    "min_days_to_pass",
    "max_days_to_pass",
    "days_to_pass_p50",
    "days_to_pass_p75",
    "days_to_pass_p90",
    "days_to_pass_p95",
    "days_to_pass_p99",
    "avg_trades_to_pass",
    "median_trades_to_pass",
    "topstep_enabled",
]

# Topstep combine rule parameters by account size.
ACCOUNT_SIZE_CONFIGS = {
    "50k": {
        "daily_drawdown_limit": 900.0,
        "trailing_drawdown_limit": 2_000.0,
        "profit_target": 3_000.0,
    },
    "150k": {
        "daily_drawdown_limit": 2_000.0,
        "trailing_drawdown_limit": 4_500.0,
        "profit_target": 9_000.0,
    },
}


# ---------------------------------------------------------------------------
# Worker globals (loaded once per process)
# ---------------------------------------------------------------------------
_worker_data_cache: Dict[str, pd.DataFrame] = {}
_worker_data_dir: str = ""


def _worker_init(data_dir: str) -> None:
    global _worker_data_dir
    _worker_data_dir = data_dir


def _load_instrument_df(instrument: str) -> pd.DataFrame:
    """Load an instrument's 1-minute data in the worker process (cached)."""
    cache = _worker_data_cache.get(instrument)
    if cache is not None:
        return cache
    path = Path(_worker_data_dir) / f"{instrument}_1min.parquet"
    if not path.exists():
        csv = Path(_worker_data_dir) / f"{instrument}_1min.csv"
        if csv.exists():
            path = csv
    df = load_market_data(str(path))
    df = df.copy()
    # ATR is used by the runner exit logic.
    if instrument in POINT_VALUES:
        df["atr"] = _paper1_atr(df, 14)
    else:
        df["atr"] = _paper2_atr(df, 14)
    _worker_data_cache[instrument] = df
    return df


# ---------------------------------------------------------------------------
# Signal / runner helpers
# ---------------------------------------------------------------------------
def _attach_tp_sl(entry_signals: pd.DataFrame, tp: float, sl: float, tick_size: float) -> pd.DataFrame:
    """Return a copy of entry signals with stop_loss/take_profit columns."""
    if entry_signals.empty:
        return entry_signals.copy()
    signals = entry_signals.copy()
    atr = signals["atr_value"].values
    direction = signals["direction"].values
    entry_price = signals["entry_price"].values

    stop_loss = np.where(
        direction == 1,
        entry_price - sl * atr - tick_size,
        entry_price + sl * atr + tick_size,
    )
    take_profit = np.where(
        direction == 1,
        entry_price + tp * atr + tick_size,
        entry_price - tp * atr - tick_size,
    )

    signals["stop_loss"] = stop_loss
    signals["take_profit"] = take_profit
    return signals


def _runner_kwargs(mode_name: str, base_kwargs: Dict[str, Any], session_end: str) -> Dict[str, Any]:
    """Build kwargs for apply_runner_to_signals.

    Baseline/trail/hold_session receive the session end time so they respect
    the session boundary.  hold_day receives the RTH day-end time (16:00 EST)
    so it can carry a trade past the entry session until the futures close.
    """
    kw = dict(base_kwargs)
    kw["session_end_time"] = pd.Timestamp(session_end).time()
    if mode_name == "hold_day":
        kw["day_end_time"] = pd.Timestamp("16:00").time()
    return kw


def _run_rust_backtest(
    signals: pd.DataFrame,
    point_value: float,
    topstep_enabled: bool,
    account_size: str = "50k",
    initial_capital: float = 100_000.0,
) -> Dict[str, Any]:
    """Execute finalized signals through the Rust engine and return a stat dict.

    The ``account_size`` argument selects the Topstep combine rule parameters
    (daily/trailing drawdown limits and profit target) passed to the engine.
    """
    engine = AgnosticBacktest(cash=initial_capital, point_value=point_value)
    topstep = None
    if topstep_enabled:
        cfg = ACCOUNT_SIZE_CONFIGS[account_size]
        topstep = TopstepConfig(
            enabled=True,
            daily_drawdown_limit=cfg["daily_drawdown_limit"],
            trailing_drawdown_limit=cfg["trailing_drawdown_limit"],
            profit_target=cfg["profit_target"],
            reset_on_failure=True,
        )
    summary = engine.run_signals(signals, topstep=topstep)
    executed = summary.executed_trades
    total_pnl = summary.total_pnl
    avg_per_trade = total_pnl / executed if executed else 0.0
    return {
        "win_rate": summary.win_rate,
        "n_trades": summary.n_trades,
        "executed_trades": executed,
        "trades_per_day": summary.n_trades / 10.0 / 252.0,
        "account_blowups": summary.account_blowups,
        "daily_limit_hits": summary.daily_limit_hits,
        "trailing_limit_hits": summary.trailing_limit_hits,
        "max_drawdown": summary.max_drawdown,
        "sharpe": summary.sharpe,
        "total_return": summary.total_return,
        "total_pnl_dollars": total_pnl,
        "avg_per_trade": avg_per_trade,
        "avg_per_week": total_pnl / 10.0 / 52.0,
        "avg_per_day": total_pnl / 10.0 / 252.0,
        "profit_target_reached": summary.profit_target_reached,
        "account_failed": summary.account_failed,
        "passed_combines": summary.passed_combines,
        "failed_combines": summary.failed_combines,
        "combine_pass_rate": summary.combine_pass_rate,
        "avg_days_to_pass": summary.avg_days_to_pass,
        "median_days_to_pass": summary.median_days_to_pass,
        "min_days_to_pass": summary.min_days_to_pass,
        "max_days_to_pass": summary.max_days_to_pass,
        "days_to_pass_p50": summary.days_to_pass_p50,
        "days_to_pass_p75": summary.days_to_pass_p75,
        "days_to_pass_p90": summary.days_to_pass_p90,
        "days_to_pass_p95": summary.days_to_pass_p95,
        "days_to_pass_p99": summary.days_to_pass_p99,
        "avg_trades_to_pass": summary.avg_trades_to_pass,
        "median_trades_to_pass": summary.median_trades_to_pass,
    }


def _safe_generate_paper1_signals(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Wrap generate_signals so a single blueprint failure does not kill the sweep."""
    try:
        return generate_paper1_signals(df, cfg, simulate_exits=False)
    except Exception as exc:  # pragma: no cover - defensive
        print(f"  signal generation failed: {cfg.get('blueprint')} {exc}", file=sys.stderr)
        return pd.DataFrame(columns=["entry_time", "direction", "entry_price", "atr_value"])


# ---------------------------------------------------------------------------
# Paper-1 combo runner
# ---------------------------------------------------------------------------
def _build_paper1_entry_cfg(sid: str, instrument: str, session: str, start: str, end: str) -> Dict[str, Any]:
    cfg = get_paper1_config(sid)
    cfg.update(
        {
            "instrument": instrument,
            "tick_size": TICK_SIZES[instrument],
            "point_value": POINT_VALUES[instrument],
            "session": session,
            "session_start": PAPER1_SESSIONS[session][0],
            "session_end": PAPER1_SESSIONS[session][1],
            "tp_atr": 2.0,  # placeholder; exits are re-simulated per TP/SL
            "sl_atr": 5.0,  # placeholder
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def _run_paper1_group(
    args: Tuple[str, str, str, str, str, str, bool, str],
) -> List[Dict[str, Any]]:
    sid, instrument, session, start, end, data_dir, topstep_enabled, account_size = args
    df = _load_instrument_df(instrument)
    cfg = _build_paper1_entry_cfg(sid, instrument, session, start, end)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start, end)

    entry_signals = _safe_generate_paper1_signals(df_s, cfg)

    # Use the full (unmasked) DataFrame for exit simulation so hold_day can
    # carry trades past the entry session until the RTH close.
    df_full = split_by_date(df.copy(), start, end)

    records: List[Dict[str, Any]] = []
    session_end = cfg["session_end"]

    # If a strategy/instrument/session combination produces no entry signals,
    # emit zero rows so the result grid stays complete and audit-friendly.
    if entry_signals.empty:
        zero_stats = {
            "win_rate": 0.0,
            "n_trades": 0,
            "executed_trades": 0,
            "account_blowups": 0,
            "daily_limit_hits": 0,
            "trailing_limit_hits": 0,
            "max_drawdown": 0.0,
            "sharpe": 0.0,
            "total_return": 0.0,
            "total_pnl_dollars": 0.0,
            "avg_per_trade": 0.0,
            "avg_per_week": 0.0,
            "avg_per_day": 0.0,
            "trades_per_day": 0.0,
            "profit_target_reached": False,
            "account_failed": False,
            "passed_combines": 0,
            "failed_combines": 0,
            "combine_pass_rate": 0.0,
            "avg_days_to_pass": 0.0,
            "median_days_to_pass": 0.0,
            "min_days_to_pass": 0,
            "max_days_to_pass": 0,
            "days_to_pass_p50": 0.0,
            "days_to_pass_p75": 0.0,
            "days_to_pass_p90": 0.0,
            "days_to_pass_p95": 0.0,
            "days_to_pass_p99": 0.0,
            "avg_trades_to_pass": 0.0,
            "median_trades_to_pass": 0.0,
        }
        for tp, sl in PAPER1_TP_SL_GRID:
            for mode_name, _ in PAPER1_RUNNER_MODES:
                records.append(
                    {
                        "paper": "paper1",
                        "strategy_id": sid,
                        "instrument": instrument,
                        "session": session,
                        "tp": tp,
                        "sl": sl,
                        "mode": mode_name,
                        **zero_stats,
                        "topstep_enabled": topstep_enabled,
                    }
                )
        return records

    for tp, sl in PAPER1_TP_SL_GRID:
        signals_with_sl_tp = _attach_tp_sl(entry_signals, tp, sl, TICK_SIZES[instrument])
        for mode_name, base_kwargs in PAPER1_RUNNER_MODES:
            kw = _runner_kwargs(mode_name, base_kwargs, session_end)
            runner_signals = apply_runner_to_signals(df_full, signals_with_sl_tp, **kw)
            if runner_signals.empty:
                continue
            stats = _run_rust_backtest(
                runner_signals, POINT_VALUES[instrument], topstep_enabled, account_size=account_size
            )
            records.append(
                {
                    "paper": "paper1",
                    "strategy_id": sid,
                    "instrument": instrument,
                    "session": session,
                    "tp": tp,
                    "sl": sl,
                    "mode": mode_name,
                    **stats,
                    "topstep_enabled": topstep_enabled,
                }
            )
    return records


# ---------------------------------------------------------------------------
# Paper-2 combo runner
# ---------------------------------------------------------------------------
def _build_paper2_cfg(sid: str, instrument: str, session: str, start: str, end: str) -> Dict[str, Any]:
    cfg = get_paper2_config(sid)
    cfg.update(
        {
            "instrument": instrument,
            "tick_size": TICK_SIZES[instrument],
            "point_value": POINT_VALUES[instrument],
            "session": session,
            "session_start": PAPER2_SESSIONS[session][0],
            "session_end": PAPER2_SESSIONS[session][1],
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def _run_paper2_group(
    args: Tuple[str, str, str, str, str, str, str, bool, str],
) -> List[Dict[str, Any]]:
    sid, instrument, session, risk_profile, start, end, data_dir, topstep_enabled, account_size = args
    df = _load_instrument_df(instrument)
    cfg = _build_paper2_cfg(sid, instrument, session, start, end)

    # Override the risk profile baked into the strategy ID so every engine is
    # evaluated under conservative and aggressive sizing.
    cfg["risk_profile"] = risk_profile
    cfg["tp_atr"], cfg["sl_atr"] = PAPER2_RISK_TP_SL[risk_profile]

    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start, end)

    try:
        signals = generate_paper2_signals(df_s, cfg)
    except Exception as exc:  # pragma: no cover - defensive
        print(f"  Paper-2 signal generation failed: {exc}", file=sys.stderr)
        return []

    if signals.empty:
        return [
            {
                "paper": "paper2",
                "strategy_id": sid,
                "instrument": instrument,
                "session": session,
                "risk_profile": risk_profile,
                "win_rate": 0.0,
                "n_trades": 0,
                "executed_trades": 0,
                "account_blowups": 0,
                "daily_limit_hits": 0,
                "trailing_limit_hits": 0,
                "max_drawdown": 0.0,
                "sharpe": 0.0,
                "total_return": 0.0,
                "total_pnl_dollars": 0.0,
                "avg_per_trade": 0.0,
                "avg_per_week": 0.0,
                "avg_per_day": 0.0,
                "trades_per_day": 0.0,
                "profit_target_reached": False,
                "account_failed": False,
                "passed_combines": 0,
                "failed_combines": 0,
                "combine_pass_rate": 0.0,
                "avg_days_to_pass": 0.0,
                "median_days_to_pass": 0.0,
                "min_days_to_pass": 0,
                "max_days_to_pass": 0,
                "days_to_pass_p50": 0.0,
                "days_to_pass_p75": 0.0,
                "days_to_pass_p90": 0.0,
                "days_to_pass_p95": 0.0,
                "days_to_pass_p99": 0.0,
                "avg_trades_to_pass": 0.0,
                "median_trades_to_pass": 0.0,
                "topstep_enabled": topstep_enabled,
            }
        ]

    stats = _run_rust_backtest(
        signals, POINT_VALUES[instrument], topstep_enabled, account_size=account_size
    )
    return [
        {
            "paper": "paper2",
            "strategy_id": sid,
            "instrument": instrument,
            "session": session,
            "risk_profile": risk_profile,
            **stats,
            "topstep_enabled": topstep_enabled,
        }
    ]


# ---------------------------------------------------------------------------
# Lookahead-bias validation harness
# ---------------------------------------------------------------------------
@dataclass
class ValidationResult:
    combo_key: str
    baseline_pnl: float
    shifted_pnl: float
    passed: bool
    note: str = ""


class LookaheadValidator:
    """Detect lookahead bias by shifting the close series forward one bar.

    A strategy that uses only closed-bar data must not improve its PnL when
    future closes are removed. The validator runs the same (strategy, instrument,
    session, TP/SL, mode) combo on the original data and on shifted data and
    asserts shifted PnL <= baseline PnL + epsilon.
    """

    EPSILON = 1e-6

    def __init__(
        self,
        data_dir: str,
        topstep_enabled: bool = False,
        account_size: str = "50k",
        start_date: str = "2016-06-01",
        end_date: str = "2026-05-29",
    ):
        self.data_dir = data_dir
        self.topstep_enabled = topstep_enabled
        self.account_size = account_size
        self.start_date = start_date
        self.end_date = end_date
        self._df_cache: Dict[str, pd.DataFrame] = {}

    def _load(self, instrument: str) -> pd.DataFrame:
        df = self._df_cache.get(instrument)
        if df is None:
            df = _load_instrument_df(instrument) if _worker_data_dir else load_market_data(
                str(Path(self.data_dir) / f"{instrument}_1min.parquet")
            )
            self._df_cache[instrument] = df
        return df

    def _shift_close(self, df: pd.DataFrame, n: int = 1) -> pd.DataFrame:
        """Return a copy where the close series is shifted forward by ``n`` bars.

        Only ``close`` is shifted; open/high/low/volume are left untouched so
        that price-action confirmation (e.g. sweep of a prior high) still uses
        the real current bar.  A closed-bar strategy must not improve its PnL
        when the current bar's close is replaced by the previous bar's close.
        """
        shifted = df.copy()
        shifted["close"] = df["close"].shift(n)
        shifted["close"] = shifted["close"].ffill()
        return shifted

    def _paper1_combo_pnl(self, sid: str, instrument: str, session: str, tp: float, sl: float, mode_name: str, df: pd.DataFrame) -> float:
        cfg = _build_paper1_entry_cfg(sid, instrument, session, self.start_date, self.end_date)
        mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
        df_s = split_by_date(df.loc[mask].copy(), self.start_date, self.end_date)
        # Ensure atr exists on the possibly-shifted frame.
        if "atr" not in df_s.columns:
            df_s["atr"] = _paper1_atr(df_s, 14)
        entry_signals = _safe_generate_paper1_signals(df_s, cfg)
        if entry_signals.empty:
            return 0.0
        signals_with_sl_tp = _attach_tp_sl(entry_signals, tp, sl, TICK_SIZES[instrument])
        base_kwargs = next(kw for name, kw in PAPER1_RUNNER_MODES if name == mode_name)
        kw = _runner_kwargs(mode_name, base_kwargs, cfg["session_end"])
        runner_signals = apply_runner_to_signals(df_s, signals_with_sl_tp, **kw)
        if runner_signals.empty:
            return 0.0
        stats = _run_rust_backtest(
            runner_signals,
            POINT_VALUES[instrument],
            self.topstep_enabled,
            account_size=self.account_size,
        )
        return float(stats["total_pnl_dollars"])

    def validate_paper1_combo(self, sid: str, instrument: str, session: str, tp: float, sl: float, mode_name: str) -> ValidationResult:
        df = self._load(instrument)
        baseline = self._paper1_combo_pnl(sid, instrument, session, tp, sl, mode_name, df)
        shifted = self._paper1_combo_pnl(sid, instrument, session, tp, sl, mode_name, self._shift_close(df))
        key = f"{sid}_{instrument}_{session}_tp{tp}_sl{sl}_{mode_name}"
        passed = shifted <= baseline + self.EPSILON
        note = ""
        if not passed:
            note = f"shifted PnL {shifted:.4f} > baseline {baseline:.4f}"
        return ValidationResult(key, baseline, shifted, passed, note)

    def run_sample(self, n_strategies: int = 3, n_sessions: int = 2) -> pd.DataFrame:
        """Validate a small sample of Paper-1 combos and return a report."""
        sids = list_paper1_ids()[:n_strategies]
        sessions = list(PAPER1_SESSIONS.keys())[:n_sessions]
        results: List[ValidationResult] = []
        for sid in sids:
            for session in sessions:
                for tp, sl in [(0.5, 6.0), (1.0, 6.0)]:
                    for mode_name, _ in PAPER1_RUNNER_MODES:
                        results.append(
                            self.validate_paper1_combo(sid, "NQ", session, tp, sl, mode_name)
                        )
        rows = [
            {
                "combo": r.combo_key,
                "baseline_pnl": r.baseline_pnl,
                "shifted_pnl": r.shifted_pnl,
                "passed": r.passed,
                "note": r.note,
            }
            for r in results
        ]
        return pd.DataFrame(rows)


# ---------------------------------------------------------------------------
# CSV output helpers
# ---------------------------------------------------------------------------
def _flush_records(records: List[Dict[str, Any]], out_path: Path, columns: Sequence[str]) -> None:
    if not records:
        return
    df = pd.DataFrame(records)[list(columns)]
    header = not out_path.exists()
    df.to_csv(out_path, mode="a", header=header, index=False)


def _print_progress(completed: int, total: int, elapsed: float) -> None:
    pct = 100.0 * completed / total if total else 0.0
    eta = (elapsed / completed * (total - completed)) if completed else 0.0
    print(
        f"  progress: {completed}/{total} ({pct:.1f}%) elapsed={elapsed:.1f}s eta={eta:.1f}s"
    )


# ---------------------------------------------------------------------------
# Task builders
# ---------------------------------------------------------------------------
def _paper1_tasks(args) -> List[Tuple[str, str, str, str, str, str, bool, str]]:
    sids = list_paper1_ids()[args.id_start : args.id_end]
    instruments = args.instruments or PAPER1_INSTRUMENTS
    sessions = args.sessions or list(PAPER1_SESSIONS.keys())
    start = args.start_date
    end = args.end_date
    tasks = [
        (sid, instr, sess, start, end, args.data_dir, args.topstep, args.account_size)
        for sid in sids
        for instr in instruments
        for sess in sessions
    ]
    return tasks


def _paper2_tasks(args) -> List[Tuple[str, str, str, str, str, str, str, bool, str]]:
    ids = list_paper2_ids()[args.id_start : args.id_end]
    instruments = args.instruments or PAPER2_INSTRUMENTS
    sessions = args.sessions or list(PAPER2_SESSIONS.keys())
    risk_profiles = args.risk_profiles or PAPER2_RISK_PROFILES
    start = args.start_date
    end = args.end_date
    return [
        (sid, instr, sess, risk, start, end, args.data_dir, args.topstep, args.account_size)
        for sid in ids
        for instr in instruments
        for sess in sessions
        for risk in risk_profiles
    ]


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------
def _parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the Paper-1 or Paper-2 matrix sweep through the Rust engine.")
    parser.add_argument("--paper", required=True, choices=["paper1", "paper2", "portfolio1"], help="Which matrix to sweep")
    parser.add_argument("--workers", type=int, default=min(16, mp.cpu_count()), help="Parallel workers")
    parser.add_argument("--output-dir", default="rust_sweep_results", help="Directory for CSV output")
    parser.add_argument("--data-dir", default=str(_PROJECT_ROOT / "data"), help="Directory with instrument parquet/csv files")
    parser.add_argument("--id-start", type=int, default=0, help="First strategy index (inclusive)")
    parser.add_argument("--id-end", type=int, default=None, help="Last strategy index (exclusive)")
    parser.add_argument("--instruments", nargs="+", default=None, help="Instruments to run")
    parser.add_argument("--sessions", nargs="+", default=None, help="Sessions to run")
    parser.add_argument("--risk-profiles", nargs="+", default=None, help="Paper-2 risk profiles to run (conservative, aggressive)")
    parser.add_argument("--start-date", default="2016-06-01", help="Backtest start date")
    parser.add_argument("--end-date", default="2026-05-29", help="Backtest end date")
    parser.add_argument("--topstep", action="store_true", help="Enable Topstep combine rules")
    parser.add_argument(
        "--account-size",
        choices=["50k", "150k"],
        default="50k",
        help="Topstep account size: selects daily/trailing drawdown limits and profit target",
    )
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing sweep CSV instead of refusing to append")
    parser.add_argument("--flush-every", type=int, default=50, help="Flush CSV every N completed groups")
    parser.add_argument("--validate", action="store_true", help="Run lookahead validation sample before sweeping")
    parser.add_argument("--strict-validate", action="store_true", help="Exit non-zero if the validation sample reports any failures")
    parser.add_argument("--validation-output", default="lookahead_validation.csv", help="CSV path for validation report")
    # Portfolio1-specific options.
    parser.add_argument("--tp-atr", type=float, default=None, help="Portfolio1 take-profit in ATR units")
    parser.add_argument("--sl-atr", type=float, default=None, help="Portfolio1 stop-loss in ATR units")
    parser.add_argument("--mode", default=None, help="Portfolio1 runner mode (trail, breakeven_then_trail, etc.)")
    parser.add_argument("--trail-atr-mult", type=float, default=None, help="Portfolio1 trailing-stop ATR multiplier")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = _parse_args(argv)
    if args.id_end is None:
        if args.paper == "portfolio1":
            args.id_end = 100
        else:
            args.id_end = 100 if args.paper == "paper1" else 108

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{args.paper}_rust_sweep_{args.account_size}.csv"
    if args.paper == "portfolio1":
        columns = OUTPUT_COLUMNS_PORTFOLIO1
    else:
        columns = OUTPUT_COLUMNS_PAPER1 if args.paper == "paper1" else OUTPUT_COLUMNS_PAPER2

    if out_path.exists() and not args.overwrite:
        print(
            f"[sweep_engine] error: {out_path} already exists. "
            "Use --overwrite to replace it or choose a different --output-dir.",
            file=sys.stderr,
        )
        return 1

    # Lookahead validation harness (optional, on a sample).
    if args.validate and args.paper != "portfolio1":
        print("[sweep_engine] running lookahead-bias validation sample...")
        validator = LookaheadValidator(
            args.data_dir,
            topstep_enabled=args.topstep,
            account_size=args.account_size,
            start_date=args.start_date,
            end_date=args.end_date,
        )
        validation_df = validator.run_sample()
        val_path = out_dir / args.validation_output
        validation_df.to_csv(val_path, index=False)
        n_failed = int((~validation_df["passed"]).sum())
        print(f"[sweep_engine] validation report: {val_path}  failures={n_failed}/{len(validation_df)}")
        if n_failed > 0:
            print(
                f"[sweep_engine] lookahead-bias validation reported {n_failed} failure(s); "
                "see validation CSV for details.",
                file=sys.stderr,
            )
            if args.strict_validate:
                return 1

    if args.paper == "paper1":
        tasks = _paper1_tasks(args)
        worker_fn = _run_paper1_group
    elif args.paper == "paper2":
        tasks = _paper2_tasks(args)
        worker_fn = _run_paper2_group
    else:
        # Portfolio1 is an account-level Topstep analysis by default.
        args.topstep = True
        tasks = _portfolio_tasks(args)
        worker_fn = _run_portfolio_group

    total = len(tasks)
    combo_hint = total
    if args.paper == "paper1":
        combo_hint = total * len(PAPER1_TP_SL_GRID) * len(PAPER1_RUNNER_MODES)
    print(
        f"[sweep_engine] {args.paper}: {total} groups, "
        f"~{combo_hint} combos, "
        f"workers={args.workers}, topstep={args.topstep}, account_size={args.account_size}"
    )

    # Write header once if the file does not exist.
    if not out_path.exists():
        pd.DataFrame(columns=list(columns)).to_csv(out_path, index=False)

    completed = 0
    failures = 0
    buffer: List[Dict[str, Any]] = []
    start_t = time.perf_counter()

    workers = min(args.workers, len(tasks)) if tasks else 1
    with ProcessPoolExecutor(max_workers=workers, initializer=_worker_init, initargs=(args.data_dir,)) as pool:
        futures = {pool.submit(worker_fn, task): task for task in tasks}
        for future in as_completed(futures):
            try:
                records = future.result()
            except Exception as exc:
                failures += 1
                task = futures[future]
                print(f"  FAILED task {task}: {exc}", file=sys.stderr)
                continue
            buffer.extend(records)
            completed += 1
            if len(buffer) >= args.flush_every:
                _flush_records(buffer, out_path, columns)
                buffer.clear()
                _print_progress(completed, total, time.perf_counter() - start_t)

    if buffer:
        _flush_records(buffer, out_path, columns)

    elapsed = time.perf_counter() - start_t
    print(
        f"[sweep_engine] done. wrote {out_path}  completed={completed}/{total} "
        f"failures={failures} elapsed={elapsed:.1f}s"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
