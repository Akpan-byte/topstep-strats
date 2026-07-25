# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Smoke tests for Kasen ORB generate_signals.
#   - Verifies required columns, default_params, and non-overlapping trades
#     on a small synthetic two-day 1-minute dataset.
# WHY: Fast regression check for the Kasen ORB assignment.

import numpy as np
import pandas as pd
import pytest

from topstep_strats.strategies.kasen_orb import default_params, generate_signals


def _make_1m_day(utc_start: pd.Timestamp, n_bars: int = 391) -> pd.DataFrame:
    """Build a 1-minute day of synthetic NQ bars ending at 16:00 ET."""
    timestamps = pd.date_range(utc_start, periods=n_bars, freq="min", tz="UTC")
    wick = 0.1
    close = 100.0 + np.zeros(n_bars)
    high = close + wick
    low = close - wick
    open_ = close.copy()
    volume = np.ones(n_bars) * 100

    # Opening range 09:30-09:45 ET => first 15 bars: high=101, low=99.
    open_[0:15] = 100.0
    high[0:15] = 101.0
    low[0:15] = 99.0
    close[0:15] = 100.0

    # Breakout after the opening range.
    close[15:30] = np.linspace(100.5, 102.0, 15)
    high[15:30] = close[15:30] + wick
    low[15:30] = close[15:30] - wick

    # Retest back to midpoint (100) with bullish close.
    close[30:45] = np.linspace(99.5, 100.5, 15)
    low[30:45] = close[30:45] - wick
    high[30:45] = close[30:45] + wick

    # Trend to take-profit.
    close[45:] = np.linspace(101.0, 105.0, n_bars - 45)
    high[45:] = close[45:] + wick
    low[45:] = close[45:] - wick

    return pd.DataFrame(
        {"open": open_, "high": high, "low": low, "close": close, "volume": volume},
        index=timestamps,
    )


def _make_two_day_data() -> pd.DataFrame:
    # 09:30 ET = 14:30 UTC during EST (January).
    day1 = _make_1m_day(pd.Timestamp("2024-01-02 14:30:00", tz="UTC"))
    day2 = _make_1m_day(pd.Timestamp("2024-01-03 14:30:00", tz="UTC"))
    # Make day2 a short setup by inverting the move and recomputing wicks.
    wick = 0.1
    day2["close"] = 100.0 - (day2["close"] - 100.0)
    day2["open"] = 100.0 - (day2["open"] - 100.0)
    day2["high"] = np.maximum(day2["open"], day2["close"]) + wick
    day2["low"] = np.minimum(day2["open"], day2["close"]) - wick
    # Restore the opening-range candle high/low so the ORB levels are well-defined.
    day2.iloc[:15, day2.columns.get_loc("open")] = 100.0
    day2.iloc[:15, day2.columns.get_loc("close")] = 100.0
    day2.iloc[:15, day2.columns.get_loc("high")] = 101.0
    day2.iloc[:15, day2.columns.get_loc("low")] = 99.0
    return pd.concat([day1, day2])


@pytest.fixture
def sample_df() -> pd.DataFrame:
    return _make_two_day_data()


def test_default_params_returns_dict():
    p = default_params()
    assert isinstance(p, dict)
    assert "reward_ratio" in p
    assert "orb_timeframe" in p


def test_generate_signals_columns(sample_df: pd.DataFrame):
    signals = generate_signals(sample_df)
    expected = [
        "entry_time",
        "direction",
        "entry_price",
        "stop_loss",
        "take_profit",
        "exit_time",
        "exit_price",
        "pnl",
        "exit_reason",
    ]
    assert list(signals.columns) == expected


def test_generate_signals_non_overlapping(sample_df: pd.DataFrame):
    signals = generate_signals(sample_df)
    assert not signals.empty, "Expected at least one Kasen ORB trade"

    # No trade should start before the previous trade has exited.
    for i in range(1, len(signals)):
        prev_exit = signals.iloc[i - 1]["exit_time"]
        curr_entry = signals.iloc[i]["entry_time"]
        assert curr_entry >= prev_exit, (
            f"Overlapping trades at row {i}: previous exits {prev_exit}, "
            f"current enters {curr_entry}"
        )


def test_generate_signals_pnl_sign_matches_direction(sample_df: pd.DataFrame):
    signals = generate_signals(sample_df)
    for _, row in signals.iterrows():
        if row["direction"] == 1:
            assert row["exit_price"] >= row["entry_price"]
        elif row["direction"] == -1:
            assert row["exit_price"] <= row["entry_price"]


def test_generate_signals_respects_params(sample_df: pd.DataFrame):
    # A very wide target should force an exit only at session end.
    signals = generate_signals(sample_df, params={"reward_ratio": 50.0})
    assert not signals.empty
    # All exits should be session_end because TP is far away.
    assert (signals["exit_reason"] == "session_end").all()


def test_empty_input():
    empty = pd.DataFrame(
        columns=["open", "high", "low", "close", "volume"]
    ).set_index(pd.DatetimeIndex([], tz="UTC"))
    signals = generate_signals(empty)
    assert signals.empty
