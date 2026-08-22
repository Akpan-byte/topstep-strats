# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Smoke test for the breakeven_then_trail runner mode.
#   - Builds a synthetic price series where a long trade hits TP and then
#     reverses hard; asserts the exit is at or above the TP price, not the
#     original stop-loss.
# WHY: Fast, deterministic verification that the breakeven floor works.

#!/usr/bin/env python3
"""Smoke test for breakeven_then_trail runner mode."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from scripts.runner_exit_numba import apply_runner_to_signals


def make_synthetic_df() -> pd.DataFrame:
    """Build a 1-minute OHLCV series with a known ATR of ~2 points."""
    idx = pd.date_range("2024-01-02 10:00", periods=30, freq="min", tz="America/New_York")
    base = 100.0
    # Prices drift up, hit TP at bar 5, then reverse below TP.
    closes = np.array(
        [100, 101, 102, 103, 104, 110, 109, 108, 107, 106,
         105, 104, 103, 102, 101, 100, 99, 98, 97, 96,
         95, 94, 93, 92, 91, 90, 89, 88, 87, 86]
    )
    opens = np.roll(closes, 1)
    opens[0] = base
    highs = np.maximum(opens, closes) + 0.5
    lows = np.minimum(opens, closes) - 0.5
    volume = np.full(len(idx), 100, dtype=np.int64)
    df = pd.DataFrame(
        {"open": opens, "high": highs, "low": lows, "close": closes, "volume": volume},
        index=idx,
    )
    df.index = df.index.tz_convert("UTC")
    # ATR ~2.0 for the first several bars.
    df["atr"] = pd.Series(np.full(len(df), 2.0), index=df.index)
    return df


def test_breakeven_then_trail_exits_at_or_above_tp():
    df = make_synthetic_df()
    signals = pd.DataFrame(
        {
            "entry_time": [pd.Timestamp("2024-01-02 10:00", tz="America/New_York")],
            "direction": [1],
            "entry_price": [100.0],
            "stop_loss": [90.0],
            "take_profit": [110.0],
        }
    )

    trades = apply_runner_to_signals(
        df, signals, mode="breakeven_then_trail", trail_atr_mult=0.5
    )
    assert len(trades) == 1, f"expected 1 trade, got {len(trades)}"
    exit_price = float(trades.iloc[0]["exit_price"])
    exit_reason = trades.iloc[0]["exit_reason"]
    # The breakeven floor guarantees exit at or above the TP price.
    assert exit_price >= 110.0, f"expected exit >= 110 (TP), got {exit_price}"
    assert exit_reason == "trail_stop", f"expected trail_stop, got {exit_reason}"
    print(f"PASS: breakeven_then_trail exited at {exit_price:.2f} ({exit_reason})")


def test_baseline_exits_at_tp():
    df = make_synthetic_df()
    signals = pd.DataFrame(
        {
            "entry_time": [pd.Timestamp("2024-01-02 10:00", tz="America/New_York")],
            "direction": [1],
            "entry_price": [100.0],
            "stop_loss": [90.0],
            "take_profit": [110.0],
        }
    )
    trades = apply_runner_to_signals(df, signals, mode="baseline")
    assert len(trades) == 1
    assert trades.iloc[0]["exit_reason"] == "tp"
    print("PASS: baseline exits at tp as expected")


if __name__ == "__main__":
    test_baseline_exits_at_tp()
    test_breakeven_then_trail_exits_at_or_above_tp()
