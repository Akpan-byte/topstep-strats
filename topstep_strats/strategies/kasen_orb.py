# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Implemented Kasen ORB strategy generate_signals for NQ 1-minute data.
#   - Added default_params with configurable session, orb, stop/target, and
#     confirmation modes.
#   - Falls back to a local pandas resample when topstep_strats.data is not yet
#     present, while preferring the shared resample_timeframe interface.
# WHY: Provides the Kasen ORB leg of the TopStep parallel backtest project.

from __future__ import annotations

from typing import Any

import numpy as np
import pandas as pd


def default_params() -> dict[str, Any]:
    """Default parameters for the Kasen ORB strategy.

    Returns
    -------
    dict
        Configurable strategy parameters.
    """
    return {
        "orb_timeframe": "15m",          # opening-range timeframe
        "session_start": "09:30",        # NY session start (ET)
        "session_end": "16:00",          # RTH end (ET)
        "tz": "America/New_York",        # timezone used to identify session
        "breakout_confirmation": "close",  # 'close' or 'wick'
        "retest_confirmation": "close",    # 'close' or 'wick'
        "stop_loss_mode": "opposite",      # 'opposite' range side
        "target_mode": "risk_reward",      # 'risk_reward' or 'opposite'
        "reward_ratio": 2.0,
        "stop_first": True,                # if TP/SL hit same bar, use SL
        "one_trade_per_day": True,
    }


def _resample_timeframe(df: pd.DataFrame, timeframe: str) -> pd.DataFrame:
    """Resample to a higher timeframe, preferring the shared data module."""
    try:
        # Prefer the shared project interface if it is available.
        from topstep_strats.data import resample_timeframe as _shared_resample

        return _shared_resample(df, timeframe)
    except Exception:
        # Fallback to a plain pandas resample.
        rule = timeframe.replace("m", "min")
        return (
            df.resample(rule)
            .agg({"open": "first", "high": "max", "low": "min", "close": "last", "volume": "sum"})
            .dropna()
        )


def generate_signals(df_1m: pd.DataFrame, params: dict[str, Any] | None = None) -> pd.DataFrame:
    """Generate Kasen ORB signals from 1-minute NQ data.

    Rules implemented per session day:
      1. Mark the high/low of the first opening-range candle (default 09:30-09:45 ET).
      2. Wait for a breakout above the high or below the low.
      3. Wait for price to retest the midpoint of that opening range.
      4. Enter long/short on midpoint-retest confirmation.
      5. Exit at the configured stop-loss or take-profit level, or at session end.

    Parameters
    ----------
    df_1m : pd.DataFrame
        1-minute bars with columns open/high/low/close/volume and a UTC datetime index.
    params : dict, optional
        Override any default parameter.

    Returns
    -------
    pd.DataFrame
        Trades with columns:
        entry_time, direction, entry_price, stop_loss, take_profit,
        exit_time, exit_price, pnl, exit_reason.
    """
    cfg = default_params()
    if params:
        cfg.update(params)

    if df_1m.empty:
        return _empty_signals()

    df = df_1m.copy()
    required = {"open", "high", "low", "close", "volume"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    # Ensure the index is timezone-aware UTC.
    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    df.index = df.index.tz_convert("UTC")

    # Convert to the session timezone for rule evaluation.
    tz = cfg["tz"]
    et_index = df.index.tz_convert(tz)
    df["_et"] = et_index
    df["_date"] = et_index.date
    df["_time"] = et_index.time

    orb_tf = cfg["orb_timeframe"]
    session_start = pd.Timestamp(cfg["session_start"]).time()
    session_end = pd.Timestamp(cfg["session_end"]).time()
    breakout_conf = cfg["breakout_confirmation"]
    retest_conf = cfg["retest_confirmation"]
    reward_ratio = float(cfg["reward_ratio"])
    stop_first = bool(cfg["stop_first"])

    # Build opening ranges from the higher timeframe in the session timezone.
    df_et = df.set_index("_et")
    orb_bars = _resample_timeframe(df_et, orb_tf)
    orb_bars["_date"] = orb_bars.index.date
    orb_bars["_time"] = orb_bars.index.time

    # The opening-range candle is the one that starts at session_start.
    opening_candles = orb_bars[orb_bars["_time"] == session_start]
    trades: list[dict[str, Any]] = []

    for date, orb in opening_candles.iterrows():
        date_val = orb["_date"]
        orb_high = float(orb["high"])
        orb_low = float(orb["low"])
        midpoint = (orb_high + orb_low) / 2.0

        # Session bars for this date, after the opening range ends.
        day_mask = (df["_date"] == date_val) & (df["_time"] > session_start)
        day_bars = df.loc[day_mask]
        if day_bars.empty:
            continue

        state = "flat"  # flat | break_long | break_short | in_trade
        entry_time = None
        entry_price = None
        direction = 0
        stop_loss = None
        take_profit = None

        for ts_utc, row in day_bars.iterrows():
            high = float(row["high"])
            low = float(row["low"])
            close = float(row["close"])
            time = row["_time"]

            if time >= session_end:
                # Force exit at the close if still in a trade.
                if state == "in_trade":
                    exit_price = close
                    pnl = direction * (exit_price - entry_price)
                    trades.append(
                        {
                            "entry_time": entry_time,
                            "direction": direction,
                            "entry_price": entry_price,
                            "stop_loss": stop_loss,
                            "take_profit": take_profit,
                            "exit_time": ts_utc,
                            "exit_price": exit_price,
                            "pnl": pnl,
                            "exit_reason": "session_end",
                        }
                    )
                break

            if state == "flat":
                if (breakout_conf == "close" and close > orb_high) or (
                    breakout_conf == "wick" and high > orb_high
                ):
                    state = "break_long"
                elif (breakout_conf == "close" and close < orb_low) or (
                    breakout_conf == "wick" and low < orb_low
                ):
                    state = "break_short"
                continue

            if state == "break_long":
                # Retest: price touches the midpoint and closes back above it.
                touched = low <= midpoint
                confirmed = close > midpoint
                if retest_conf == "wick":
                    confirmed = high > midpoint
                if touched and confirmed:
                    entry_time = ts_utc
                    entry_price = close
                    direction = 1
                    stop_loss = orb_low
                    risk = entry_price - stop_loss
                    if cfg["target_mode"] == "opposite":
                        take_profit = orb_high
                    else:
                        take_profit = entry_price + risk * reward_ratio
                    state = "in_trade"
                continue

            if state == "break_short":
                touched = high >= midpoint
                confirmed = close < midpoint
                if retest_conf == "wick":
                    confirmed = low < midpoint
                if touched and confirmed:
                    entry_time = ts_utc
                    entry_price = close
                    direction = -1
                    stop_loss = orb_high
                    risk = stop_loss - entry_price
                    if cfg["target_mode"] == "opposite":
                        take_profit = orb_low
                    else:
                        take_profit = entry_price - risk * reward_ratio
                    state = "in_trade"
                continue

            if state == "in_trade":
                sl_hit = (direction == 1 and low <= stop_loss) or (
                    direction == -1 and high >= stop_loss
                )
                tp_hit = (direction == 1 and high >= take_profit) or (
                    direction == -1 and low <= take_profit
                )

                if sl_hit and tp_hit:
                    # Conservative fill: stop-first assumption.
                    exit_price = stop_loss
                    reason = "sl"
                elif sl_hit:
                    exit_price = stop_loss
                    reason = "sl"
                elif tp_hit:
                    exit_price = take_profit
                    reason = "tp"
                else:
                    continue

                pnl = direction * (exit_price - entry_price)
                trades.append(
                    {
                        "entry_time": entry_time,
                        "direction": direction,
                        "entry_price": entry_price,
                        "stop_loss": stop_loss,
                        "take_profit": take_profit,
                        "exit_time": ts_utc,
                        "exit_price": exit_price,
                        "pnl": pnl,
                        "exit_reason": reason,
                    }
                )
                state = "done"
                if cfg.get("one_trade_per_day", True):
                    break

    if not trades:
        return _empty_signals()

    result = pd.DataFrame(trades)
    result = result[
        [
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
    ]
    return result


def _empty_signals() -> pd.DataFrame:
    """Return an empty signals DataFrame with the required schema."""
    return pd.DataFrame(
        columns=[
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
    )
