# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created tests/test_nitro_crt.py.
#   - Smoke tests for default_params, required columns, non-overlapping trades,
#     and basic long/short behavior on synthetic 1m data.
# WHY: Verify Nitro CRT strategy satisfies the shared interface before CI runs.

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from topstep_strats.strategies import nitro_crt


def _make_crt_sample() -> pd.DataFrame:
    """Build synthetic 1m data with a clean long CRT setup in the second hour."""
    # RTH 09:30-10:30 ET = 14:30-15:30 UTC in January (EST, UTC-5).
    base = pd.Timestamp("2024-01-02 14:30:00", tz="UTC")
    times = pd.date_range(base, periods=120, freq="1min")
    df = pd.DataFrame(
        index=times,
        columns=["open", "high", "low", "close", "volume"],
        dtype=float,
    )

    # First hour: range high = 20050, low = 19950.
    for i, t in enumerate(times[:60]):
        o = 20000.0 + i * 0.10
        c = o + 0.30
        h = max(o, c) + 0.10
        l = min(o, c) - 0.10
        df.loc[t] = [o, h, l, c, 100.0]

    df.loc[times[0], ["open", "high", "low", "close"]] = [19970.0, 19980.0, 19950.0, 19960.0]
    df.loc[times[30], ["open", "high", "low", "close"]] = [20030.0, 20050.0, 20020.0, 20040.0]

    # Second hour: sweep the 19950 low, reverse, and run to the 20050 opposing level.
    for i, t in enumerate(times[60:120], start=60):
        o = 19980.0 + (i - 60) * 0.20
        c = o + 0.20
        h = max(o, c) + 0.10
        l = min(o, c) - 0.10
        df.loc[t] = [o, h, l, c, 100.0]

    sweep = times[70]
    df.loc[sweep, ["open", "high", "low", "close"]] = [19970.0, 19980.0, 19948.0, 19955.0]

    # Keep price above the stop level (19950 - buffer) until target is hit.
    for t in times[71:85]:
        df.loc[t, "low"] = max(df.loc[t, "low"], 19952.0)

    target_bar = times[85]
    df.loc[target_bar, ["open", "high", "low", "close"]] = [19995.0, 20060.0, 19990.0, 20055.0]

    return df


def _make_short_sample() -> pd.DataFrame:
    """Build synthetic 1m data with a clean short CRT setup in the second hour."""
    base = pd.Timestamp("2024-01-02 14:30:00", tz="UTC")
    times = pd.date_range(base, periods=120, freq="1min")
    df = pd.DataFrame(
        index=times,
        columns=["open", "high", "low", "close", "volume"],
        dtype=float,
    )

    for i, t in enumerate(times[:60]):
        o = 20000.0 + i * 0.10
        c = o - 0.30
        h = max(o, c) + 0.10
        l = min(o, c) - 0.10
        df.loc[t] = [o, h, l, c, 100.0]

    df.loc[times[0], ["open", "high", "low", "close"]] = [20030.0, 20050.0, 20000.0, 20010.0]
    df.loc[times[30], ["open", "high", "low", "close"]] = [19960.0, 19980.0, 19940.0, 19950.0]

    for i, t in enumerate(times[60:120], start=60):
        o = 20000.0 - (i - 60) * 0.20
        c = o - 0.20
        h = max(o, c) + 0.10
        l = min(o, c) - 0.10
        df.loc[t] = [o, h, l, c, 100.0]

    sweep = times[70]
    df.loc[sweep, ["open", "high", "low", "close"]] = [20030.0, 20052.0, 20020.0, 20045.0]

    for t in times[71:85]:
        df.loc[t, "high"] = min(df.loc[t, "high"], 20048.0)

    target_bar = times[85]
    df.loc[target_bar, ["open", "high", "low", "close"]] = [19960.0, 19965.0, 19930.0, 19935.0]

    return df


def test_default_params():
    p = nitro_crt.default_params()
    assert isinstance(p, dict)
    assert "htf_timeframe" in p
    assert "entry_timeframe" in p
    assert p["target_mode"] in ("opposite", "fixed_rr")


def test_empty_dataframe():
    df = pd.DataFrame(
        columns=["open", "high", "low", "close", "volume"]
    ).set_index(pd.to_datetime([]))
    sig = nitro_crt.generate_signals(df)
    assert list(sig.columns) == [
        "entry_time", "direction", "entry_price", "stop_loss",
        "take_profit", "exit_time", "exit_price", "pnl", "exit_reason",
    ]
    assert sig.empty


def test_long_crt_columns_and_exit_reason():
    df = _make_crt_sample()
    sig = nitro_crt.generate_signals(df)
    assert not sig.empty
    for col in [
        "entry_time", "direction", "entry_price", "stop_loss",
        "take_profit", "exit_time", "exit_price", "pnl", "exit_reason",
    ]:
        assert col in sig.columns
    assert (sig["direction"] == 1).all()
    # Synthetic design: the long trade should hit the opposing CRT high target.
    assert sig.iloc[0]["exit_reason"] == "target"


def test_short_crt_columns_and_exit_reason():
    df = _make_short_sample()
    sig = nitro_crt.generate_signals(df)
    assert not sig.empty
    assert (sig["direction"] == -1).all()
    assert sig.iloc[0]["exit_reason"] == "target"


def test_trades_do_not_overlap():
    df = _make_crt_sample()
    sig = nitro_crt.generate_signals(df)
    if len(sig) > 1:
        for i in range(1, len(sig)):
            assert sig.iloc[i]["entry_time"] >= sig.iloc[i - 1]["exit_time"]


def test_fixed_rr_mode():
    df = _make_crt_sample()
    sig = nitro_crt.generate_signals(df, params={"target_mode": "fixed_rr", "risk_reward": 1.5})
    assert not sig.empty
    # In fixed_rr mode the take-profit is derived from entry and stop.
    row = sig.iloc[0]
    expected_tp = row["entry_price"] + 1.5 * (row["entry_price"] - row["stop_loss"])
    assert abs(row["take_profit"] - expected_tp) < 1e-6


def test_session_only_excludes_out_of_session():
    out_times = pd.date_range(
        "2024-01-02 22:00:00", periods=60, freq="1min", tz="UTC"
    )
    df = pd.DataFrame(
        {
            "open": np.linspace(20000, 20100, 60),
            "high": np.linspace(20010, 20110, 60),
            "low": np.linspace(19990, 20090, 60),
            "close": np.linspace(20005, 20105, 60),
            "volume": [100.0] * 60,
        },
        index=out_times,
    )
    sig = nitro_crt.generate_signals(df, params={"session_only": True})
    assert sig.empty
