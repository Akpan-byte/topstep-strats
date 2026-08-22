"""Integration tests for the Rust/PyO3 backtest engine.

Requires the extension to be built, e.g.:
    cd engine_rust && maturin develop
"""

from __future__ import annotations

import csv
import os
import tempfile

import pandas as pd
import pytest

from agnostic_backtest import AgnosticBacktest, Bar, Signal


class BuyAndHold:
    """Go long on the first bar and hold until the end."""

    def __init__(self) -> None:
        self._entered = False

    def on_bar(self, bar: Bar) -> Signal | None:
        if not self._entered:
            self._entered = True
            return Signal(direction=1)
        return None


def _write_sample_csv(path: str) -> None:
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ns", "open", "high", "low", "close", "volume"])
        # Uptrend with gaps to make next-bar-open execution unambiguous.
        writer.writerow([1, 100.0, 105.0, 95.0, 110.0, 100])
        writer.writerow([2, 120.0, 125.0, 115.0, 120.0, 100])
        writer.writerow([3, 130.0, 135.0, 125.0, 130.0, 100])


def _write_sample_parquet(path: str) -> None:
    df = pd.DataFrame(
        {
            "timestamp_ns": [1, 2, 3],
            "open": [100.0, 120.0, 130.0],
            "high": [105.0, 125.0, 135.0],
            "low": [95.0, 115.0, 125.0],
            "close": [110.0, 120.0, 130.0],
            "volume": [100, 100, 100],
        }
    )
    df.to_parquet(path, index=False)


def test_buy_and_hold_no_lookahead():
    """Strategy sees bar 0 close (110) and goes long. Fill must be at bar 1 open (120)."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "sample.csv")
        _write_sample_csv(path)

        engine = AgnosticBacktest(cash=10_000.0)
        summary = engine.run_csv(path, BuyAndHold())

        trades = engine.trades()
        assert len(trades) == 1, trades
        entry_ts, exit_ts, entry_price, exit_price, qty, pnl = trades[0]

        # No lookahead: entry at next bar open, not the signal bar close.
        assert entry_price == 120.0, f"expected entry at next open 120.0, got {entry_price}"
        # Final close flattens the position.
        assert exit_price == 130.0, f"expected exit at final close 130.0, got {exit_price}"
        assert qty == 1, f"expected qty 1, got {qty}"
        assert pnl == 10.0, f"expected pnl 10.0, got {pnl}"
        assert summary.n_trades == 1
        assert summary.total_return > 0


def test_buy_and_hold_parquet():
    """Parquet input path must produce the same results as CSV."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "sample.parquet")
        _write_sample_parquet(path)

        engine = AgnosticBacktest(cash=10_000.0)
        summary = engine.run_parquet(path, BuyAndHold())

        trades = engine.trades()
        assert len(trades) == 1, trades
        entry_ts, exit_ts, entry_price, exit_price, qty, pnl = trades[0]
        assert entry_price == 120.0
        assert exit_price == 130.0
        assert pnl == 10.0
        assert summary.n_trades == 1


class FlipFlop:
    """Alternate long/short every bar to exercise direction changes."""

    def __init__(self) -> None:
        self._idx = 0

    def on_bar(self, bar: Bar) -> Signal | None:
        self._idx += 1
        return Signal(direction=1 if self._idx % 2 == 1 else -1)


def test_flip_flop_generates_trades():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "sample.csv")
        _write_sample_csv(path)

        engine = AgnosticBacktest(cash=10_000.0)
        summary = engine.run_csv(path, FlipFlop())

        assert summary.n_trades >= 1
        assert len(engine.equity_curve()) > 1


class AlwaysLong:
    def on_bar(self, bar: Bar) -> Signal | None:
        return Signal(direction=1)


def test_session_filter_skips_out_of_session_bars():
    """Only bars inside 01:00-03:00 UTC can trade; the 03:00 bar flattens the position."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "session.csv")
        with open(path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["timestamp_ns", "open", "high", "low", "close", "volume"])
            # Each row is one hour apart in nanoseconds.
            hour_ns = 3_600_000_000_000
            writer.writerow([hour_ns, 100.0, 101.0, 99.0, 100.0, 100])      # 01:00 UTC
            writer.writerow([2 * hour_ns, 100.0, 101.0, 99.0, 100.0, 100])  # 02:00 UTC
            writer.writerow([3 * hour_ns, 100.0, 101.0, 99.0, 100.0, 100])  # 03:00 UTC

        engine = AgnosticBacktest(
            cash=10_000.0,
            timezone="UTC",
            session_start="01:00",
            session_end="03:00",
        )
        summary = engine.run_csv(path, AlwaysLong())

        trades = engine.trades()
        assert len(trades) == 1, trades
        entry_ts, exit_ts, entry_price, exit_price, qty, pnl = trades[0]
        assert entry_ts == 2 * hour_ns
        # Flattened at the open of the first out-of-session bar.
        assert exit_ts == 3 * hour_ns


def test_daily_loss_limit_triggers_failure():
    """A large intraday drop breaches the daily loss limit and resets the account."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "drop.csv")
        with open(path, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["timestamp_ns", "open", "high", "low", "close", "volume"])
            writer.writerow([1, 100.0, 100.0, 100.0, 100.0, 100])
            writer.writerow([2, 100.0, 100.0, 50.0, 50.0, 100])
            writer.writerow([3, 50.0, 50.0, 50.0, 50.0, 100])

        engine = AgnosticBacktest(
            cash=10_000.0,
            daily_loss_limit=500.0,
            reset_on_failure=True,
            point_value=100.0,
        )
        summary = engine.run_csv(path, AlwaysLong())
        assert summary.failures >= 1
        assert summary.daily_limit_hits >= 1


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
