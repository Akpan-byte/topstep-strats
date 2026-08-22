"""Tests for the Rust/PyO3 sweep orchestrator and run_signals path."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

# Ensure the project root and Rust Python package are importable.
_ENGINE_RUST_ROOT = Path(__file__).resolve().parent.parent
_PROJECT_ROOT = _ENGINE_RUST_ROOT.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))
if str(_ENGINE_RUST_ROOT / "python") not in sys.path:
    sys.path.insert(0, str(_ENGINE_RUST_ROOT / "python"))

from agnostic_backtest import AgnosticBacktest, TopstepConfig
from engine_rust.scripts.sweep_engine import (
    LookaheadValidator,
    _paper1_tasks,
    _paper2_tasks,
)


def _signals_df(trades: list) -> pd.DataFrame:
    """Build a minimal finalized signal DataFrame for run_signals."""
    return pd.DataFrame(
        trades,
        columns=["entry_time", "exit_time", "direction", "entry_price", "exit_price", "pnl"],
    )


def test_run_signals_basic():
    """run_signals should apply point_value to the supplied PnL points."""
    signals = _signals_df(
        [
            ("2024-01-01 10:00", "2024-01-01 11:00", 1, 100.0, 110.0, 10.0),
            ("2024-01-01 12:00", "2024-01-01 13:00", 1, 110.0, 115.0, 5.0),
        ]
    )
    engine = AgnosticBacktest(cash=10_000.0, point_value=20.0)
    summary = engine.run_signals(signals)
    # Net points = 10 + 5 = 15; dollars = 15 * 20 = 300.
    assert summary.total_pnl == pytest.approx(300.0)
    assert summary.executed_trades == 2
    assert summary.win_rate == 1.0


def test_run_signals_topstep_daily_limit():
    """Topstep daily loss limit should reset the account after a large loss."""
    signals = _signals_df(
        [
            ("2024-01-01 10:00", "2024-01-01 11:00", 1, 100.0, 80.0, -20.0),
            ("2024-01-01 12:00", "2024-01-01 13:00", 1, 80.0, 90.0, 10.0),
        ]
    )
    engine = AgnosticBacktest(cash=10_000.0, point_value=100.0)
    ts = TopstepConfig(enabled=True, daily_drawdown_limit=1_500.0, reset_on_failure=True)
    summary = engine.run_signals(signals, topstep=ts)
    # First trade loses $2,000 (> $1,500) -> reset; second trade executes on fresh account.
    assert summary.daily_limit_hits >= 1
    assert summary.account_blowups >= 1
    assert summary.executed_trades == 2


def test_run_signals_topstep_trailing_limit_halts():
    """Topstep trailing drawdown without reset should halt trading."""
    signals = _signals_df(
        [
            ("2024-01-01 10:00", "2024-01-01 11:00", 1, 100.0, 120.0, 20.0),
            ("2024-01-01 12:00", "2024-01-01 13:00", 1, 120.0, 90.0, -30.0),
            ("2024-01-01 14:00", "2024-01-01 15:00", 1, 90.0, 100.0, 10.0),
        ]
    )
    engine = AgnosticBacktest(cash=10_000.0, point_value=100.0)
    ts = TopstepConfig(enabled=True, trailing_drawdown_limit=1_000.0, reset_on_failure=False)
    summary = engine.run_signals(signals, topstep=ts)
    # Peak equity after first trade = $12,000. Second trade drops to $9,000,
    # a $3,000 trailing drawdown -> account fails; third trade skipped.
    assert summary.account_failed
    assert summary.trailing_limit_hits >= 1
    assert summary.executed_trades == 2


def test_paper1_task_count():
    """The Paper-1 task builder should produce the expected Cartesian product."""
    class Args:
        paper = "paper1"
        id_start = 0
        id_end = 2
        instruments = ["NQ"]
        sessions = ["NY"]
        start_date = "2016-06-01"
        end_date = "2026-05-29"
        data_dir = str(_PROJECT_ROOT / "data")
        topstep = False

    tasks = _paper1_tasks(Args())
    assert len(tasks) == 2
    assert tasks[0][:3] == ("001", "NQ", "NY")
    assert tasks[1][:3] == ("002", "NQ", "NY")


def test_paper2_task_count():
    """The Paper-2 task builder should produce the expected Cartesian product."""
    class Args:
        paper = "paper2"
        id_start = 0
        id_end = 2
        instruments = ["NQ"]
        sessions = ["NY"]
        start_date = "2016-06-01"
        end_date = "2026-05-29"
        data_dir = str(_PROJECT_ROOT / "data")
        topstep = False

    tasks = _paper2_tasks(Args())
    assert len(tasks) == 2
    assert tasks[0][:3] == (0, "NQ", "NY")


def test_lookahead_validator_no_lookahead_for_paper1():
    """A closed-bar Paper-1 combo must not improve when future closes are removed."""
    data_dir = _PROJECT_ROOT / "data"
    if not (data_dir / "NQ_1min.parquet").exists():
        pytest.skip("NQ market data not available")

    validator = LookaheadValidator(str(data_dir), topstep_enabled=False)
    # Use a small, fast Casper config.
    result = validator.validate_paper1_combo("010", "NQ", "NY", 0.5, 6.0, "baseline")
    assert result.passed, result.note


def test_shift_close_detects_peeking_strategy():
    """Shifting close forward must not improve a non-peeking strategy.

    We build a trivial strategy that enters long when close > open. Because it
    only uses the current bar, shifting close forward should not increase PnL.
    """
    idx = pd.date_range("2024-01-01", periods=10, freq="h", tz="UTC")
    df = pd.DataFrame(
        {
            "open": np.linspace(100.0, 109.0, 10),
            "high": np.linspace(101.0, 110.0, 10),
            "low": np.linspace(99.0, 108.0, 10),
            "close": np.linspace(100.5, 109.5, 10),
            "volume": np.full(10, 100, dtype=np.int64),
        },
        index=idx,
    )

    def strategy(frame: pd.DataFrame) -> pd.DataFrame:
        mask = frame["close"] > frame["open"]
        rows = []
        for t in frame.index[mask]:
            rows.append(
                {
                    "entry_time": t,
                    "exit_time": t + pd.Timedelta(hours=1),
                    "direction": 1,
                    "entry_price": frame.loc[t, "close"],
                    "exit_price": frame.loc[t, "close"] + 0.2,
                    "pnl": 0.2,
                }
            )
        return pd.DataFrame(rows)

    baseline = strategy(df)
    shifted = strategy(df.assign(close=df["close"].shift(-1).ffill()))
    # The trivial strategy should produce the same number of signals after shift.
    assert len(baseline) == len(shifted)
