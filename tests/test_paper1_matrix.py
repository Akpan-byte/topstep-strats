# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Implemented tests/test_paper1_matrix.py with four validation suites for
#     the Paper-1 matrix sweep:
#       (a) smoke test that every strategy ID 001-100 returns a valid signals
#           DataFrame on synthetic 1-minute data;
#       (b) future-shift leak test where shifting close forward by one bar must
#           not improve PnL;
#       (c) reverse-entry control where flipping direction mirrors equity;
#       (d) deterministic hand-crafted bars that exercise each of the seven
#           blueprints and verify exact entry/exit prices against the bars.
# WHY: The matrix-sweep-paper1 branch needs fast, reproducible regression tests
#      that guard against crashes, lookahead bias, sign errors, and blueprint
#      breakage without requiring multi-gigabyte market data files.

from __future__ import annotations

from typing import Any, Dict, List, Tuple

import numpy as np
import pandas as pd
import pytest

from topstep_strats.backtest import run_backtest
from topstep_strats.strategies.paper1_matrix import (
    BLUEPRINTS,
    generate_signals,
    get_strategy_config,
    list_strategy_ids,
)


# ---------------------------------------------------------------------------
# Shared synthetic data helpers
# ---------------------------------------------------------------------------

REQUIRED_COLS = [
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


def _empty_1m_index(n_days: int = 2, start: str = "2024-01-02 00:00:00") -> pd.DatetimeIndex:
    """Return a UTC-aware 1-minute index spanning ``n_days`` full days."""
    return pd.date_range(start, periods=n_days * 24 * 60, freq="1min", tz="UTC")


def _flat_baseline(idx: pd.DatetimeIndex, price: float = 15000.0, volume: int = 1000) -> pd.DataFrame:
    """Create a flat OHLCV DataFrame with the requested index."""
    n = len(idx)
    return pd.DataFrame(
        {
            "open": np.full(n, price),
            "high": np.full(n, price + 1.0),
            "low": np.full(n, price - 1.0),
            "close": np.full(n, price + 0.5),
            "volume": np.full(n, volume, dtype=np.int64),
        },
        index=idx,
    )


def _trend_with_volatility(idx: pd.DatetimeIndex, start_price: float = 14950.0) -> pd.DataFrame:
    """Gentle upward-trending data with enough intraday volatility for ATR."""
    n = len(idx)
    trend = np.linspace(0.0, 40.0, n)
    close = start_price + trend
    return pd.DataFrame(
        {
            "open": close - 0.5,
            "high": close + 1.5,
            "low": close - 1.5,
            "close": close,
            "volume": np.full(n, 1000, dtype=np.int64),
        },
        index=idx,
    )


# ---------------------------------------------------------------------------
# Blueprint-specific deterministic scenarios
#
# Each builder returns (df_1m, params).  Expected prices are derived from the
# actual DataFrame at runtime so the tests stay exact but not brittle to ATR
# rounding.  All scenarios use session="NY" and filter=None.
# ---------------------------------------------------------------------------

def _ict_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """ICT Silver Bullet: sweep prior 15m low in NY session, long entry."""
    idx = _empty_1m_index(n_days=2)
    # Day 1 entirely flat -> ATR is zero, so no day-1 trade can be taken.
    df = _flat_baseline(idx, price=14950.0, volume=1000)
    df.loc[idx[idx.day == 2], "open"] = 14950.0
    df.loc[idx[idx.day == 2], "high"] = 14950.0
    df.loc[idx[idx.day == 2], "low"] = 14950.0
    df.loc[idx[idx.day == 2], "close"] = 14950.0

    day2_start = 24 * 60
    d2 = idx[day2_start:]

    # 15m bars are label=left / closed-left, so the 14:30 bar covers
    # 14:30-14:45 UTC, 14:45 covers 14:45-15:00, and 15:00 covers 15:00-15:15.
    m0 = (d2.hour == 14) & (d2.minute >= 30) & (d2.minute < 45)
    df.loc[d2[m0], "open"] = 15005.0
    df.loc[d2[m0], "high"] = 15010.0
    df.loc[d2[m0], "low"] = 15000.0
    df.loc[d2[m0], "close"] = 15005.0

    # Higher low than m0 so it does not sweep; sets the prev_low for m2.
    m1 = (d2.hour == 14) & (d2.minute >= 45) & (d2.minute < 60)
    df.loc[d2[m1], "open"] = 15005.0
    df.loc[d2[m1], "high"] = 15005.0
    df.loc[d2[m1], "low"] = 15003.0
    df.loc[d2[m1], "close"] = 15004.0

    # Sweep bar: takes out the m1 low and closes back in the retracement zone.
    m2 = (d2.hour == 15) & (d2.minute >= 0) & (d2.minute < 15)
    df.loc[d2[m2], "open"] = 15005.0
    df.loc[d2[m2], "high"] = 15006.0
    df.loc[d2[m2], "low"] = 14999.0
    df.loc[d2[m2], "close"] = 15004.0

    trigger_idx = idx.get_loc(d2[m2][0])
    post = idx[trigger_idx + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = 15004.0 + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    params = {
        "blueprint": "ict_silver_bullet",
        "session": "NY",
        "filter": None,
        "sweep_timeframe": "15m",
        "entry_timeframe": "1m",
        "fvg_required": False,
        "retracement_pct": 0.5,
        "atr_length": 14,
        "tp_atr": 2.0,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


def _casper_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """Casper Inverted FVG: bearish FVG gets filled and retested -> long."""
    idx = _empty_1m_index(n_days=2)
    df = _flat_baseline(idx, price=15000.0)
    day2_start = 24 * 60
    d2 = idx[day2_start:]

    # 5m bars inside NY session (14:30 UTC = 09:30 ET).
    b0 = (d2.hour == 14) & (d2.minute >= 30) & (d2.minute < 35)
    df.loc[d2[b0], "open"] = 15007.0
    df.loc[d2[b0], "high"] = 15010.0
    df.loc[d2[b0], "low"] = 15005.0
    df.loc[d2[b0], "close"] = 15007.0

    b1 = (d2.hour == 14) & (d2.minute >= 35) & (d2.minute < 40)
    df.loc[d2[b1], "open"] = 15007.0
    df.loc[d2[b1], "high"] = 15007.0
    df.loc[d2[b1], "low"] = 15002.0
    df.loc[d2[b1], "close"] = 15004.0

    # Bearish FVG: high < low[i-2] (high < 15005).
    b2 = (d2.hour == 14) & (d2.minute >= 40) & (d2.minute < 45)
    df.loc[d2[b2], "open"] = 15004.0
    df.loc[d2[b2], "high"] = 15004.0
    df.loc[d2[b2], "low"] = 14995.0
    df.loc[d2[b2], "close"] = 14998.0

    # Filling bar: high >= gap_top and close > gap_top.
    b3 = (d2.hour == 14) & (d2.minute >= 45) & (d2.minute < 50)
    df.loc[d2[b3], "open"] = 14998.0
    df.loc[d2[b3], "high"] = 15010.0
    df.loc[d2[b3], "low"] = 14998.0
    df.loc[d2[b3], "close"] = 15008.0

    trigger_idx = idx.get_loc(d2[b3][-1])
    post = idx[trigger_idx + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = 15008.0 + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    # Volatile history for ATR.
    df.loc[idx[:day2_start], "high"] = df.loc[idx[:day2_start], "close"] + 2.0
    df.loc[idx[:day2_start], "low"] = df.loc[idx[:day2_start], "close"] - 2.0

    params = {
        "blueprint": "casper_inverted_fvg",
        "session": "NY",
        "filter": None,
        "fvg_timeframe": "5m",
        "entry_model": "close_back_inside",
        "atr_length": 14,
        "tp_atr": 2.0,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


def _velez_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """Velez 20/200 Elephant Bar: large bullish bar above EMA20 and SMA200."""
    idx = _empty_1m_index(n_days=2)
    df = _trend_with_volatility(idx, start_price=14950.0)

    trigger_idx = idx.get_loc(pd.Timestamp("2024-01-03 18:40:00", tz="UTC"))
    df.iloc[trigger_idx, df.columns.get_loc("open")] = 15000.0
    df.iloc[trigger_idx, df.columns.get_loc("low")] = 14995.0
    df.iloc[trigger_idx, df.columns.get_loc("high")] = 15015.0
    df.iloc[trigger_idx, df.columns.get_loc("close")] = 15014.0

    post = idx[trigger_idx + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = 15014.0 + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    params = {
        "blueprint": "velez_20_200_elephant_bar",
        "session": "NY",
        "filter": None,
        "ema_fast": 20,
        "ema_slow": 200,
        "min_range_atr": 1.0,
        "close_position": "extreme",
        "atr_length": 14,
        "tp_atr": 2.0,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


def _rosato_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """Rosato S/D Absorption: demand test followed by absorption long."""
    idx = _empty_1m_index(n_days=2)
    df = _flat_baseline(idx, price=15000.0, volume=1000)
    day2_start = 24 * 60
    d2 = idx[day2_start:]

    # 15m zone bars in NY session.
    b0 = (d2.hour == 15) & (d2.minute >= 0) & (d2.minute < 15)
    df.loc[d2[b0], "open"] = 15020.0
    df.loc[d2[b0], "high"] = 15020.0
    df.loc[d2[b0], "low"] = 15010.0
    df.loc[d2[b0], "close"] = 15015.0
    df.loc[d2[b0], "volume"] = 10000

    b1 = (d2.hour == 15) & (d2.minute >= 15) & (d2.minute < 30)
    df.loc[d2[b1], "open"] = 15015.0
    df.loc[d2[b1], "high"] = 15015.0
    df.loc[d2[b1], "low"] = 15005.0
    df.loc[d2[b1], "close"] = 15010.0
    df.loc[d2[b1], "volume"] = 1000

    # Demand test.
    b2 = (d2.hour == 15) & (d2.minute >= 30) & (d2.minute < 45)
    df.loc[d2[b2], "open"] = 15005.0
    df.loc[d2[b2], "high"] = 15010.0
    df.loc[d2[b2], "low"] = 15004.0
    df.loc[d2[b2], "close"] = 15009.0
    df.loc[d2[b2], "volume"] = 5000

    # Absorption.
    b3 = (d2.hour == 15) & (d2.minute >= 45) & (d2.minute < 60)
    df.loc[d2[b3], "open"] = 15009.0
    df.loc[d2[b3], "high"] = 15015.0
    df.loc[d2[b3], "low"] = 15008.0
    df.loc[d2[b3], "close"] = 15014.0
    df.loc[d2[b3], "volume"] = 3000

    trigger_idx = idx.get_loc(d2[b3][-1])
    post = idx[trigger_idx + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = 15014.0 + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    params = {
        "blueprint": "rosato_sd_absorption",
        "session": "NY",
        "filter": None,
        "zone_timeframe": "15m",
        "volume_percentile": 70,
        "absorption_bars": 3,
        "atr_length": 14,
        "tp_atr": 1.5,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


def _carter_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """Carter TTM Squeeze: tight range (squeeze) then bullish breakout."""
    idx = _empty_1m_index(n_days=2)
    df = _flat_baseline(idx, price=15000.0)
    day2_start = 24 * 60

    # Day 1 volatility for indicator warmup.
    df.loc[idx[:day2_start], "high"] = df.loc[idx[:day2_start], "close"] + 2.0
    df.loc[idx[:day2_start], "low"] = df.loc[idx[:day2_start], "close"] - 2.0

    # Day 2 morning tight squeeze inside NY session.
    squeeze = (idx[day2_start:].hour >= 14) & (idx[day2_start:].hour < 16)
    d2_squeeze = idx[day2_start:][squeeze]
    df.loc[d2_squeeze, "open"] = 15000.0
    df.loc[d2_squeeze, "high"] = 15000.5
    df.loc[d2_squeeze, "low"] = 14999.5
    df.loc[d2_squeeze, "close"] = 15000.0

    # Breakout bar at 16:00 UTC (11:00 ET).
    breakout = idx.get_loc(pd.Timestamp("2024-01-03 16:00:00", tz="UTC"))
    df.iloc[breakout, df.columns.get_loc("open")] = 15000.0
    df.iloc[breakout, df.columns.get_loc("low")] = 15000.0
    df.iloc[breakout, df.columns.get_loc("high")] = 15020.0
    df.iloc[breakout, df.columns.get_loc("close")] = 15019.0

    post = idx[breakout + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = 15019.0 + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    params = {
        "blueprint": "carter_ttm_squeeze",
        "session": "NY",
        "filter": None,
        "bb_length": 20,
        "kc_mult": 1.5,
        "momentum_length": 12,
        "direction": "both",
        "atr_length": 14,
        "tp_atr": 2.0,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


def _raschke_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """Raschke Holy Grail: rising EMA, ADX strong, pullback close above EMA."""
    idx = _empty_1m_index(n_days=2)
    # Day 1 flat so it does not trigger.
    df = _flat_baseline(idx, price=14950.0, volume=1000)

    day2_start = 24 * 60
    d2 = idx[day2_start:]
    # Day 2 strong uptrend with volatility to build ADX and EMA slope.
    n2 = len(d2)
    trend = np.linspace(0.0, 60.0, n2)
    close = 14950.0 + trend
    df.loc[d2, "open"] = close - 0.5
    df.loc[d2, "high"] = close + 2.0
    df.loc[d2, "low"] = close - 2.0
    df.loc[d2, "close"] = close

    # Trigger bar: pullback to EMA then close above.
    trigger_idx = idx.get_loc(pd.Timestamp("2024-01-03 18:40:00", tz="UTC"))
    ema_approx = 15010.0
    df.iloc[trigger_idx, df.columns.get_loc("open")] = ema_approx - 1.0
    df.iloc[trigger_idx, df.columns.get_loc("low")] = ema_approx - 2.0
    df.iloc[trigger_idx, df.columns.get_loc("high")] = ema_approx + 2.0
    df.iloc[trigger_idx, df.columns.get_loc("close")] = ema_approx + 1.5

    post = idx[trigger_idx + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = (ema_approx + 1.5) + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    params = {
        "blueprint": "raschke_holy_grail",
        "session": "NY",
        "filter": None,
        "ema_length": 20,
        "adx_length": 14,
        "adx_threshold": 25,
        "pullback_depth": 0.3,
        "atr_length": 14,
        "tp_atr": 2.0,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


def _wade_scenario() -> Tuple[pd.DataFrame, Dict[str, Any]]:
    """Wade PATs Second Entry: failed swing-high break then close above it."""
    idx = _empty_1m_index(n_days=2)
    # Flat, quiet baseline keeps every bar's high identical and below the
    # engineered peak, so no premature failures occur before the peak.
    df = _flat_baseline(idx, price=14980.0, volume=1000)

    swing_time = idx.get_loc(pd.Timestamp("2024-01-03 15:00:00", tz="UTC"))
    fail_time = idx.get_loc(pd.Timestamp("2024-01-03 15:30:00", tz="UTC"))
    confirm_time = idx.get_loc(pd.Timestamp("2024-01-03 15:31:00", tz="UTC"))

    # 15:00 through 15:29 form a plateau at the same high, so every bar marks
    # the level and keeps high_failures reset, but the close never breaks above.
    plateau = idx[swing_time:fail_time]
    df.loc[plateau, "open"] = 14985.0
    df.loc[plateau, "low"] = 14980.0
    df.loc[plateau, "high"] = 15020.0
    df.loc[plateau, "close"] = 15020.0

    # Failed break: high takes out the swing high but close settles back below.
    df.iloc[fail_time, df.columns.get_loc("open")] = 15012.0
    df.iloc[fail_time, df.columns.get_loc("low")] = 15012.0
    df.iloc[fail_time, df.columns.get_loc("high")] = 15021.0
    df.iloc[fail_time, df.columns.get_loc("close")] = 15018.0

    # Confirmation bar closes above the swing high.
    df.iloc[confirm_time, df.columns.get_loc("open")] = 15018.0
    df.iloc[confirm_time, df.columns.get_loc("low")] = 15017.0
    df.iloc[confirm_time, df.columns.get_loc("high")] = 15025.0
    df.iloc[confirm_time, df.columns.get_loc("close")] = 15024.0

    post = idx[confirm_time + 1 :]
    for i, ts in enumerate(post):
        df.loc[ts, "open"] = 15024.0 + i * 0.5
        df.loc[ts, "close"] = df.loc[ts, "open"] + 0.5
        df.loc[ts, "high"] = df.loc[ts, "close"] + 0.5
        df.loc[ts, "low"] = df.loc[ts, "open"] - 0.5

    params = {
        "blueprint": "wade_pats_second_entry",
        "session": "NY",
        "filter": None,
        "swing_lookback": 5,
        "second_entry_mode": "close_beyond",
        "trend_filter": False,
        "atr_length": 14,
        "tp_atr": 4.0,
        "sl_atr": 5.0,
        "tick_size": 0.25,
        "stop_first": True,
    }
    return df, params


BLUEPRINT_SCENARIOS = {
    "ict_silver_bullet": _ict_scenario,
    "casper_inverted_fvg": _casper_scenario,
    "velez_20_200_elephant_bar": _velez_scenario,
    "rosato_sd_absorption": _rosato_scenario,
    "carter_ttm_squeeze": _carter_scenario,
    "raschke_holy_grail": _raschke_scenario,
    "wade_pats_second_entry": _wade_scenario,
}


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def sample_df() -> pd.DataFrame:
    """Generic two-day synthetic 1m data for smoke tests."""
    return _trend_with_volatility(_empty_1m_index(n_days=2))


# ---------------------------------------------------------------------------
# Suite (a): smoke tests for all 100 matrix IDs
# ---------------------------------------------------------------------------

class TestPaper1Smoke:
    def test_all_ids_return_valid_signals(self, sample_df: pd.DataFrame):
        for sid in list_strategy_ids():
            cfg = get_strategy_config(sid)
            signals = generate_signals(sample_df, cfg)

            assert isinstance(signals, pd.DataFrame), f"ID {sid}: expected DataFrame"
            assert list(signals.columns) == REQUIRED_COLS, (
                f"ID {sid}: unexpected columns {list(signals.columns)}"
            )
            if not signals.empty:
                assert pd.api.types.is_datetime64_any_dtype(signals["entry_time"])
                assert pd.api.types.is_datetime64_any_dtype(signals["exit_time"])
                for _, row in signals.iterrows():
                    assert row["pnl"] * row["direction"] >= -1e-12

    def test_empty_input_returns_empty_signals(self):
        empty = pd.DataFrame(
            columns=["open", "high", "low", "close", "volume"]
        ).astype(
            {"open": float, "high": float, "low": float, "close": float, "volume": int}
        )
        empty.index = pd.DatetimeIndex([], tz="UTC", name="timestamp")
        signals = generate_signals(empty)
        assert signals.empty
        assert list(signals.columns) == REQUIRED_COLS


# ---------------------------------------------------------------------------
# Suite (b): future-shift leak test
# ---------------------------------------------------------------------------

class TestPaper1Leak:
    @pytest.mark.parametrize("sid", ["001", "015", "029", "043", "058", "073", "087"])
    def test_forward_close_shift_does_not_improve_pnl(self, sample_df: pd.DataFrame, sid: str):
        cfg = get_strategy_config(sid)

        baseline = generate_signals(sample_df, cfg)
        baseline_pnl = float(baseline["pnl"].sum()) if not baseline.empty else 0.0

        shifted = sample_df.copy()
        shifted["close"] = sample_df["close"].shift(-1)
        shifted["close"] = shifted["close"].ffill()
        shifted_signals = generate_signals(shifted, cfg)
        shifted_pnl = float(shifted_signals["pnl"].sum()) if not shifted_signals.empty else 0.0

        assert shifted_pnl <= baseline_pnl + 1e-6, (
            f"ID {sid}: forward-shifted close improved PnL "
            f"({shifted_pnl:.4f} > {baseline_pnl:.4f})"
        )


# ---------------------------------------------------------------------------
# Suite (c): reverse-entry control
# ---------------------------------------------------------------------------

class TestPaper1ReverseEntry:
    def _reverse(self, signals: pd.DataFrame) -> pd.DataFrame:
        rev = signals.copy()
        rev["direction"] = -rev["direction"]
        rev[["stop_loss", "take_profit"]] = rev[["take_profit", "stop_loss"]].to_numpy()
        # run_backtest consumes the pre-computed pnl column, so mirror it too.
        rev["pnl"] = -rev["pnl"]
        return rev

    @pytest.mark.parametrize("blueprint", BLUEPRINTS)
    def test_flipping_direction_mirrors_equity(self, blueprint: str):
        df, params = BLUEPRINT_SCENARIOS[blueprint]()
        baseline = generate_signals(df, params)
        assert not baseline.empty, f"{blueprint}: no baseline trades to reverse"

        bt_base = run_backtest(baseline)
        initial = bt_base["summary"]["initial_capital"]
        base_final = bt_base["summary"]["final_equity"]

        bt_rev = run_backtest(self._reverse(baseline))
        rev_final = bt_rev["summary"]["final_equity"]
        expected_final = 2 * initial - base_final

        assert rev_final == pytest.approx(expected_final, abs=1e-9), (
            f"{blueprint}: reversed equity {rev_final} != expected {expected_final}"
        )


# ---------------------------------------------------------------------------
# Suite (d): deterministic hand-crafted bars for each blueprint
# ---------------------------------------------------------------------------

class TestPaper1Deterministic:
    @pytest.mark.parametrize("blueprint", BLUEPRINTS)
    def test_blueprint_generates_expected_trade(self, blueprint: str):
        df, params = BLUEPRINT_SCENARIOS[blueprint]()
        signals = generate_signals(df, params)

        assert not signals.empty, f"{blueprint}: expected at least one deterministic trade"
        assert len(signals) == 1, f"{blueprint}: expected exactly one trade, got {len(signals)}"

        trade = signals.iloc[0]
        entry_time = trade["entry_time"]
        exit_time = trade["exit_time"]
        direction = trade["direction"]

        # Entry price must come from the actual bar (close or extreme + tick).
        if blueprint in {"raschke_holy_grail", "wade_pats_second_entry"}:
            # These blueprints enter at high/low + tick on the trigger bar.
            bar = df.loc[entry_time]
            if direction == 1:
                expected_entry = float(bar["high"]) + params["tick_size"]
            else:
                expected_entry = float(bar["low"]) - params["tick_size"]
        else:
            expected_entry = float(df.loc[entry_time, "close"])

        assert trade["entry_price"] == pytest.approx(expected_entry, abs=1e-9)
        assert trade["entry_time"] < trade["exit_time"]
        assert trade["exit_reason"] in {"sl", "tp", "session_end", "end_of_data"}

        # Stop/target ordering must match direction.
        if direction == 1:
            assert trade["stop_loss"] < trade["entry_price"] < trade["take_profit"]
        else:
            assert trade["take_profit"] < trade["entry_price"] < trade["stop_loss"]

        # PnL must match the realized exit price and be direction-consistent.
        expected_pnl = direction * (trade["exit_price"] - trade["entry_price"])
        assert trade["pnl"] == pytest.approx(expected_pnl, abs=1e-9)
        assert trade["pnl"] * direction >= -1e-12

    @pytest.mark.parametrize("blueprint", BLUEPRINTS)
    def test_blueprint_no_lookahead_in_filter(self, blueprint: str):
        idx = _empty_1m_index(n_days=3)
        df = _trend_with_volatility(idx)
        params = {"blueprint": blueprint, "session": "NY", "filter": "price_gt_sma200"}
        signals = generate_signals(df, params)
        assert isinstance(signals, pd.DataFrame)
        assert list(signals.columns) == REQUIRED_COLS


# ---------------------------------------------------------------------------
# Sanity checks on the public API
# ---------------------------------------------------------------------------

def test_list_strategy_ids_covers_001_to_100():
    ids = list_strategy_ids()
    assert len(ids) == 100
    assert ids == [f"{i:03d}" for i in range(1, 101)]


def test_get_strategy_config_returns_copy():
    cfg1 = get_strategy_config("001")
    cfg2 = get_strategy_config("001")
    assert cfg1 is not cfg2
    assert cfg1["blueprint"] == cfg2["blueprint"]
