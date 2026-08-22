# CHANGE_SUMMARY
# 2026-08-21  kilo
#   - Created topstep_strats/strategies/asian_session.py with six Asian-session
#     specific mean-reversion strategies:
#       1) Bollinger Band mean reversion
#       2) RSI oversold/overbought with confirmation
#       3) Opening range fade
#       4) VWAP deviation reversion
#       5) ATR compression + failed breakout
#       6) Asian session pivot range scalping
#   - All indicators are computed on Bar[1] (shift(1)) to avoid lookahead bias.
#   - Signals are entry-only (entry_time, direction, entry_price, atr_value) so
#     callers can cache and re-apply TP/SL grids.
# WHY: The existing Paper-1/2 strategies underperform in the tight, range-bound
#      Asian session (18:00-02:00 EST). Dedicated mean-reversion engines are
#      needed to lift the Asian session edge toward London/NY levels.

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd

from topstep_strats import data


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

STRATEGIES = [
    "bollinger_mean_reversion",
    "rsi_extreme",
    "opening_range_fade",
    "vwap_deviation",
    "atr_compression_failed_breakout",
    "pivot_range_scalp",
]


def default_params() -> Dict[str, Any]:
    """Default parameters shared by all Asian-session strategies."""
    return {
        "strategy": "bollinger_mean_reversion",
        "session_start": "18:00",
        "session_end": "02:00",
        "tz": "America/New_York",
        "one_trade_per_day": True,
        "atr_length": 14,
        "tick_size": 0.25,
    }


def generate_signals(
    df_1m: pd.DataFrame,
    params: Dict[str, Any] | None = None,
) -> pd.DataFrame:
    """Generate Asian-session entry signals.

    Parameters
    ----------
    df_1m : pd.DataFrame
        1-minute OHLCV data with a UTC DatetimeIndex.
    params : dict, optional
        Override any default parameter, including ``strategy``.

    Returns
    -------
    pd.DataFrame
        Entry-only signals with columns:
        entry_time, direction, entry_price, atr_value.
    """
    cfg = default_params()
    if params:
        cfg.update(params)

    if df_1m.empty:
        return _empty_signals()

    df = df_1m.copy()
    # Cached Numba attrs from prior runner calls contain numpy arrays that
    # break pandas __finalize__ comparisons (e.g. during pd.concat in ATR).
    df.attrs = {}
    required = {"open", "high", "low", "close", "volume"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    df.index = df.index.tz_convert("UTC")

    strategy = cfg.get("strategy", "bollinger_mean_reversion")
    if strategy == "bollinger_mean_reversion":
        return _bollinger_mean_reversion(df, cfg)
    if strategy == "rsi_extreme":
        return _rsi_extreme(df, cfg)
    if strategy == "opening_range_fade":
        return _opening_range_fade(df, cfg)
    if strategy == "vwap_deviation":
        return _vwap_deviation(df, cfg)
    if strategy == "atr_compression_failed_breakout":
        return _atr_compression_failed_breakout(df, cfg)
    if strategy == "pivot_range_scalp":
        return _pivot_range_scalp(df, cfg)
    raise ValueError(f"Unknown Asian-session strategy: {strategy!r}")


def _empty_signals() -> pd.DataFrame:
    return pd.DataFrame(
        columns=["entry_time", "direction", "entry_price", "atr_value"]
    )


# ---------------------------------------------------------------------------
# Shared helpers (closed-bar only)
# ---------------------------------------------------------------------------

def _add_session_meta(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Add local date/time/session columns without lookahead."""
    tz = cfg["tz"]
    local = df.index.tz_convert(tz)
    df = df.copy()
    df["_date"] = local.date
    df["_time"] = local.time
    df["_in_session"] = data.get_session_mask(
        df, cfg["session_start"], cfg["session_end"], tz
    )
    return df


def _true_range(df: pd.DataFrame) -> pd.Series:
    prev_close = df["close"].shift(1)
    return pd.concat(
        [
            df["high"] - df["low"],
            (df["high"] - prev_close).abs(),
            (df["low"] - prev_close).abs(),
        ],
        axis=1,
    ).max(axis=1)


def _atr(df: pd.DataFrame, length: int) -> pd.Series:
    return _true_range(df).rolling(length, min_periods=length).mean().shift(1)


def _sma(series: pd.Series, length: int) -> pd.Series:
    return series.rolling(length, min_periods=length).mean().shift(1)


def _std(series: pd.Series, length: int) -> pd.Series:
    return series.rolling(length, min_periods=length).std().shift(1)


def _rsi(series: pd.Series, length: int = 14) -> pd.Series:
    delta = series.diff()
    gain = delta.where(delta > 0, 0.0)
    loss = (-delta).where(delta < 0, 0.0)
    avg_gain = gain.rolling(length, min_periods=length).mean().shift(1)
    avg_loss = loss.rolling(length, min_periods=length).mean().shift(1)
    rs = avg_gain / avg_loss.replace(0, np.nan)
    return 100.0 - (100.0 / (1.0 + rs))


def _vwap(df: pd.DataFrame) -> pd.Series:
    """Daily anchored VWAP (closed bar only)."""
    local = df.index.tz_convert("America/New_York")
    date = local.date
    typical = (df["high"] + df["low"] + df["close"]) / 3.0
    vol = df["volume"].astype(float)
    cum_typ_vol = (typical * vol).groupby(date).cumsum().shift(1)
    cum_vol = vol.groupby(date).cumsum().shift(1)
    return cum_typ_vol / cum_vol.replace(0, np.nan)


def _entry_signal(
    entry_time: pd.Timestamp,
    direction: int,
    entry_price: float,
    atr_value: float,
) -> Dict[str, Any]:
    return {
        "entry_time": entry_time,
        "direction": direction,
        "entry_price": entry_price,
        "atr_value": atr_value,
    }


def _emit_signals(
    trades: List[Dict[str, Any]],
) -> pd.DataFrame:
    if not trades:
        return _empty_signals()
    df = pd.DataFrame(trades)
    return df[["entry_time", "direction", "entry_price", "atr_value"]]


# ---------------------------------------------------------------------------
# Strategy implementations
# ---------------------------------------------------------------------------

def _bollinger_mean_reversion(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Long when price touches the lower Bollinger Band, short when it touches
    the upper band. Uses a close back inside the band as confirmation.
    """
    length = int(cfg.get("bb_length", 20))
    std_dev = float(cfg.get("bb_std", 2.0))

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["bb_mid"] = _sma(df1["close"], length)
    df1["bb_std"] = _std(df1["close"], length)
    df1["bb_upper"] = df1["bb_mid"] + std_dev * df1["bb_std"]
    df1["bb_lower"] = df1["bb_mid"] - std_dev * df1["bb_std"]

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    upper = df1["bb_upper"].values
    lower = df1["bb_lower"].values
    atr_vals = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = (
        in_session
        & ~np.isnan(upper)
        & ~np.isnan(lower)
        & ~np.isnan(atr_vals)
        & (atr_vals > 0)
    )

    # Touch band on prior wick, current close back inside (confirmation).
    touched_lower = low <= lower
    confirmed_long = (close > lower) & touched_lower
    touched_upper = high >= upper
    confirmed_short = (close < upper) & touched_upper

    long_cond = valid & confirmed_long
    short_cond = valid & confirmed_short

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    for i in np.where(long_cond | short_cond)[0]:
        if not in_session[i]:
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue
        if long_cond[i] and short_cond[i]:
            # If both fire, skip to avoid ambiguous signal.
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr_vals[i])))
        last_date = date

    return _emit_signals(trades)


def _rsi_extreme(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Long when RSI drops below the oversold threshold and closes back above
    it; short when RSI rises above overbought and closes back below.
    """
    length = int(cfg.get("rsi_length", 14))
    oversold = float(cfg.get("rsi_oversold", 20))
    overbought = float(cfg.get("rsi_overbought", 80))

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["rsi"] = _rsi(df1["close"], length)
    df1["rsi_prev"] = df1["rsi"].shift(1)

    close = df1["close"].values
    rsi = df1["rsi"].values
    rsi_prev = df1["rsi_prev"].values
    atr_vals = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = in_session & ~np.isnan(rsi) & ~np.isnan(rsi_prev) & ~np.isnan(atr_vals) & (atr_vals > 0)

    # Cross back above oversold -> long; cross back below overbought -> short.
    long_cond = valid & (rsi_prev < oversold) & (rsi >= oversold)
    short_cond = valid & (rsi_prev > overbought) & (rsi <= overbought)

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    for i in np.where(long_cond | short_cond)[0]:
        if not in_session[i]:
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr_vals[i])))
        last_date = date

    return _emit_signals(trades)


def _opening_range_fade(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Establish the Asian opening range (first 30 minutes) and fade any
    breakout beyond that range within the session.
    """
    range_minutes = int(cfg.get("opening_range_minutes", 30))

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))

    tz = cfg["tz"]
    local_index = df1.index.tz_convert(tz)
    df1["_datetime"] = local_index

    # Build a session identifier that handles overnight sessions.
    # A session starts at session_start and may cross midnight; use the start
    # date of the session as the session key.
    session_start = pd.Timestamp(cfg["session_start"]).time()
    session_end = pd.Timestamp(cfg["session_end"]).time()
    if session_start < session_end:
        df1["_session_key"] = local_index.normalize()
    else:
        # Overnight: assign bars before midnight to the previous day's session.
        session_key = pd.Series(local_index.normalize(), index=df1.index)
        mask_pre = pd.Series(local_index.time, index=df1.index) < session_start
        session_key.loc[mask_pre] = session_key.loc[mask_pre] - pd.Timedelta(days=1)
        df1["_session_key"] = session_key.values

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    atr_vals = df1["atr"].values
    in_session = df1["_in_session"].values
    session_keys = df1["_session_key"].values
    local_dt = df1["_datetime"].values

    # Pre-compute opening range high/low per session (first N minutes).
    range_start = session_start
    range_end = (
        pd.Timestamp("2000-01-01").replace(
            hour=range_start.hour, minute=range_start.minute
        )
        + pd.Timedelta(minutes=range_minutes)
    ).time()

    or_high: Dict[Any, float] = {}
    or_low: Dict[Any, float] = {}
    or_end_idx: Dict[Any, int] = {}
    full_times = df1.index.tz_convert(tz).time
    in_or_full = full_times >= range_start
    if range_end >= range_start:
        in_or_full = in_or_full & (full_times <= range_end)
    else:
        in_or_full = in_or_full | (full_times <= range_end)
    for sk in np.unique(session_keys):
        session_mask = session_keys == sk
        or_mask = session_mask & in_or_full
        or_bars = df1[or_mask]
        if not or_bars.empty:
            or_high[sk] = float(or_bars["high"].max())
            or_low[sk] = float(or_bars["low"].min())
            or_end_idx[sk] = int(np.where(or_mask)[0][-1])

    trades: List[Dict[str, Any]] = []
    last_key: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    n = len(df1)
    for i in range(n):
        if not in_session[i]:
            continue
        sk = session_keys[i]
        if sk not in or_high:
            continue
        if i <= or_end_idx[sk]:
            continue
        if one_trade and last_key == sk:
            continue
        if not (~np.isnan(atr_vals[i]) and atr_vals[i] > 0):
            continue

        if high[i] >= or_high[sk]:
            trades.append(_entry_signal(df1.index[i], -1, float(close[i]), float(atr_vals[i])))
            last_key = sk
        elif low[i] <= or_low[sk]:
            trades.append(_entry_signal(df1.index[i], 1, float(close[i]), float(atr_vals[i])))
            last_key = sk

    return _emit_signals(trades)


def _vwap_deviation(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Revert price to the daily anchored VWAP when it deviates by more than
    ``vwap_std_mult`` standard deviations.
    """
    std_mult = float(cfg.get("vwap_std_mult", 1.5))
    std_length = int(cfg.get("vwap_std_length", 20))

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["vwap"] = _vwap(df1)
    df1["vwap_std"] = (
        (df1["close"] - df1["vwap"]).rolling(std_length, min_periods=std_length).std().shift(1)
    )

    close = df1["close"].values
    vwap = df1["vwap"].values
    vwap_std = df1["vwap_std"].values
    atr_vals = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = (
        in_session
        & ~np.isnan(vwap)
        & ~np.isnan(vwap_std)
        & (vwap_std > 0)
        & ~np.isnan(atr_vals)
        & (atr_vals > 0)
    )

    long_cond = valid & (close < vwap - std_mult * vwap_std)
    short_cond = valid & (close > vwap + std_mult * vwap_std)

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    for i in np.where(long_cond | short_cond)[0]:
        if not in_session[i]:
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr_vals[i])))
        last_date = date

    return _emit_signals(trades)


def _atr_compression_failed_breakout(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """After a period of ATR compression (tight range), fade a false breakout
    that fails to follow through.
    """
    compression_length = int(cfg.get("compression_length", 14))
    compression_mult = float(cfg.get("compression_mult", 0.6))
    lookback = int(cfg.get("breakout_lookback", 10))

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["range"] = df1["high"] - df1["low"]
    df1["atr_lookback_max"] = df1["atr"].rolling(compression_length, min_periods=compression_length).max().shift(1)
    df1["compressed"] = df1["range"] < compression_mult * df1["atr_lookback_max"]

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    atr_vals = df1["atr"].values
    compressed = df1["compressed"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)
    n = len(df1)

    for i in range(lookback + compression_length + 1, n):
        if not in_session[i]:
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue
        if not (bool(compressed[i]) and not np.isnan(atr_vals[i]) and float(atr_vals[i]) > 0):
            continue

        # Recent swing high/low up to the compression bar.
        recent_high = float(np.nanmax(high[i - lookback : i]))
        recent_low = float(np.nanmin(low[i - lookback : i]))

        # Failed breakout: price pokes above recent high but closes back below it -> short.
        if bool(high[i] > recent_high) and bool(close[i] < recent_high):
            trades.append(_entry_signal(df1.index[i], -1, float(close[i]), float(atr_vals[i])))
            last_date = date
            continue

        # Failed breakdown: price pokes below recent low but closes back above it -> long.
        if bool(low[i] < recent_low) and bool(close[i] > recent_low):
            trades.append(_entry_signal(df1.index[i], 1, float(close[i]), float(atr_vals[i])))
            last_date = date

    return _emit_signals(trades)


def _pivot_range_scalp(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Use the first portion of the Asian session to establish a pivot range
    (high/low), then fade moves to the extremes of that range.
    """
    pivot_minutes = int(cfg.get("pivot_range_minutes", 90))

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))

    tz = cfg["tz"]
    local_index = df1.index.tz_convert(tz)
    df1["_datetime"] = local_index

    session_start = pd.Timestamp(cfg["session_start"]).time()
    session_end = pd.Timestamp(cfg["session_end"]).time()
    if session_start < session_end:
        df1["_session_key"] = local_index.normalize()
    else:
        session_key = pd.Series(local_index.normalize(), index=df1.index)
        mask_pre = pd.Series(local_index.time, index=df1.index) < session_start
        session_key.loc[mask_pre] = session_key.loc[mask_pre] - pd.Timedelta(days=1)
        df1["_session_key"] = session_key.values

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    atr_vals = df1["atr"].values
    in_session = df1["_in_session"].values
    session_keys = df1["_session_key"].values

    # Build pivot range per session (first N minutes).
    pivot_start = session_start
    pivot_end = (
        pd.Timestamp("2000-01-01").replace(
            hour=pivot_start.hour, minute=pivot_start.minute
        )
        + pd.Timedelta(minutes=pivot_minutes)
    ).time()

    pivot_high: Dict[Any, float] = {}
    pivot_low: Dict[Any, float] = {}
    pivot_end_idx: Dict[Any, int] = {}
    full_times = df1.index.tz_convert(tz).time
    in_pivot_full = full_times >= pivot_start
    if pivot_end >= pivot_start:
        in_pivot_full = in_pivot_full & (full_times <= pivot_end)
    else:
        in_pivot_full = in_pivot_full | (full_times <= pivot_end)
    for sk in np.unique(session_keys):
        session_mask = session_keys == sk
        pivot_mask = session_mask & in_pivot_full
        pivot_bars = df1[pivot_mask]
        if not pivot_bars.empty:
            pivot_high[sk] = float(pivot_bars["high"].max())
            pivot_low[sk] = float(pivot_bars["low"].min())
            pivot_end_idx[sk] = int(np.where(pivot_mask)[0][-1])

    trades: List[Dict[str, Any]] = []
    last_key: Any = None
    one_trade = cfg.get("one_trade_per_day", True)
    n = len(df1)

    for i in range(n):
        if not in_session[i]:
            continue
        sk = session_keys[i]
        if sk not in pivot_high:
            continue
        if i <= pivot_end_idx[sk]:
            continue
        if one_trade and last_key == sk:
            continue
        if not (~np.isnan(atr_vals[i]) and atr_vals[i] > 0):
            continue

        if high[i] >= pivot_high[sk]:
            trades.append(_entry_signal(df1.index[i], -1, float(close[i]), float(atr_vals[i])))
            last_key = sk
        elif low[i] <= pivot_low[sk]:
            trades.append(_entry_signal(df1.index[i], 1, float(close[i]), float(atr_vals[i])))
            last_key = sk

    return _emit_signals(trades)
