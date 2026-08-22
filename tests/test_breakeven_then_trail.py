# CHANGE_SUMMARY
# 2026-08-20  coder
#   - Smoke test for the breakeven_then_trail runner mode.
#   - Verifies that after a take-profit level is touched the stop is moved to
#     the TP price and then trailed, so a subsequent reversal exits at TP or
#     better rather than at the original stop-loss.
# WHY: The new mode must lock in TP profits and never give them back.

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pandas as pd

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from scripts.runner_exit_numba import apply_runner_to_signals


def _make_1m_index(n_bars: int = 100, start: str = "2024-01-02 15:00:00") -> pd.DatetimeIndex:
    return pd.date_range(start, periods=n_bars, freq="1min", tz="UTC")


def test_breakeven_then_trail_locks_in_tp():
    """Long entry hits TP, reverses through original SL; exit is at TP."""
    idx = _make_1m_index(n_bars=20)
    entry_price = 100.0
    sl = 99.0
    tp = 102.0

    # Construct bars so bar 5 touches TP, then bars 6-19 fall back through SL.
    opens = np.full(20, entry_price)
    closes = np.full(20, entry_price)
    highs = np.full(20, entry_price + 0.2)
    lows = np.full(20, entry_price - 0.2)

    highs[5] = tp + 0.1          # TP touched
    opens[6] = tp
    closes[6] = 101.0
    highs[6] = 101.5
    lows[6] = 100.8

    for i in range(7, 20):
        opens[i] = closes[i - 1]
        closes[i] = closes[i - 1] - 0.5
        highs[i] = max(opens[i], closes[i]) + 0.1
        lows[i] = min(opens[i], closes[i]) - 0.1

    # Make sure later bars fall through the original SL.
    lows[10:] = np.minimum(lows[10:], 98.0)

    df = pd.DataFrame(
        {
            "open": opens,
            "high": highs,
            "low": lows,
            "close": closes,
            "volume": np.full(20, 1000, dtype=np.int64),
        },
        index=idx,
    )
    df["atr"] = np.full(20, 1.0)

    signals = pd.DataFrame(
        {
            "entry_time": [idx[0]],
            "direction": [1],
            "entry_price": [entry_price],
            "stop_loss": [sl],
            "take_profit": [tp],
        }
    )

    result = apply_runner_to_signals(df, signals, mode="breakeven_then_trail", trail_atr_mult=0.5)

    assert result["exit_price"].iloc[0] >= tp - 1e-9, (
        f"breakeven_then_trail should exit at or above TP ({tp}), got {result['exit_price'].iloc[0]}"
    )
    assert result["exit_reason"].iloc[0] == "trail_stop", (
        f"expected trail_stop, got {result['exit_reason'].iloc[0]}"
    )
    assert result["pnl"].iloc[0] >= tp - entry_price - 1e-9, (
        f"expected PnL >= {tp - entry_price}, got {result['pnl'].iloc[0]}"
    )


def test_breakeven_then_trail_short_locks_in_tp():
    """Short entry hits TP, reverses through original SL; exit is at TP."""
    idx = _make_1m_index(n_bars=20)
    entry_price = 100.0
    sl = 101.0
    tp = 98.0

    opens = np.full(20, entry_price)
    closes = np.full(20, entry_price)
    highs = np.full(20, entry_price + 0.2)
    lows = np.full(20, entry_price - 0.2)

    lows[5] = tp - 0.1           # TP touched
    opens[6] = tp
    closes[6] = 99.0
    highs[6] = 99.2
    lows[6] = 98.5

    for i in range(7, 20):
        opens[i] = closes[i - 1]
        closes[i] = closes[i - 1] + 0.5
        highs[i] = max(opens[i], closes[i]) + 0.1
        lows[i] = min(opens[i], closes[i]) - 0.1

    highs[10:] = np.maximum(highs[10:], 102.0)

    df = pd.DataFrame(
        {
            "open": opens,
            "high": highs,
            "low": lows,
            "close": closes,
            "volume": np.full(20, 1000, dtype=np.int64),
        },
        index=idx,
    )
    df["atr"] = np.full(20, 1.0)

    signals = pd.DataFrame(
        {
            "entry_time": [idx[0]],
            "direction": [-1],
            "entry_price": [entry_price],
            "stop_loss": [sl],
            "take_profit": [tp],
        }
    )

    result = apply_runner_to_signals(df, signals, mode="breakeven_then_trail", trail_atr_mult=0.5)

    assert result["exit_price"].iloc[0] <= tp + 1e-9, (
        f"short breakeven_then_trail should exit at or below TP ({tp}), got {result['exit_price'].iloc[0]}"
    )
    assert result["exit_reason"].iloc[0] == "trail_stop"
    assert result["pnl"].iloc[0] >= entry_price - tp - 1e-9


def test_breakeven_floor_vs_plain_trail():
    """Plain trail can give back TP; breakeven_then_trail floors at TP."""
    idx = _make_1m_index(n_bars=20)
    entry_price = 100.0
    sl = 99.0
    tp = 102.0

    opens = np.full(20, entry_price)
    closes = np.full(20, entry_price)
    highs = np.full(20, entry_price + 0.2)
    lows = np.full(20, entry_price - 0.2)

    highs[5] = tp + 0.1
    # After TP, a sharp gap down would put plain trail stop below TP.
    opens[6] = 101.5
    closes[6] = 100.5
    highs[6] = 101.8
    lows[6] = 100.2

    for i in range(7, 20):
        opens[i] = closes[i - 1]
        closes[i] = closes[i - 1] - 0.3
        highs[i] = max(opens[i], closes[i]) + 0.1
        lows[i] = min(opens[i], closes[i]) - 0.1

    df = pd.DataFrame(
        {
            "open": opens,
            "high": highs,
            "low": lows,
            "close": closes,
            "volume": np.full(20, 1000, dtype=np.int64),
        },
        index=idx,
    )
    df["atr"] = np.full(20, 2.0)

    signals = pd.DataFrame(
        {
            "entry_time": [idx[0]],
            "direction": [1],
            "entry_price": [entry_price],
            "stop_loss": [sl],
            "take_profit": [tp],
        }
    )

    plain = apply_runner_to_signals(df, signals, mode="trail", trail_atr_mult=0.5)
    be = apply_runner_to_signals(df, signals, mode="breakeven_then_trail", trail_atr_mult=0.5)

    # The breakeven exit should be at least as good as the plain trail exit.
    assert be["exit_price"].iloc[0] >= plain["exit_price"].iloc[0] - 1e-9
    assert be["exit_price"].iloc[0] >= tp - 1e-9


if __name__ == "__main__":
    test_breakeven_then_trail_locks_in_tp()
    test_breakeven_then_trail_short_locks_in_tp()
    test_breakeven_floor_vs_plain_trail()
    print("breakeven_then_trail smoke tests passed")
