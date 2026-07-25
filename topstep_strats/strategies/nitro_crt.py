# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created topstep_strats/strategies/nitro_crt.py.
#   - Implements Nitro CRT: higher-timeframe high/low sweep + reversal entry on
#     a lower timeframe, targeting the opposing CRT level.
# WHY: Shared strategy interface for the topstep-strats parallel backtest project.

from __future__ import annotations

from typing import Dict, Any

import pandas as pd

from topstep_strats import data


def default_params() -> Dict[str, Any]:
    """Default parameters for the Nitro CRT strategy.

    Defaults are tuned for Topstep-style evaluation: only the first qualifying
    CRT setup of the NY RTH session is taken, matching Nitro's "first CRT of
    the New York session" rule.
    """
    return {
        "htf_timeframe": "1h",          # higher timeframe that sets CRT levels
        "entry_timeframe": "1m",        # lower timeframe used for entry trigger
        "risk_reward": 1.5,             # used when target_mode == 'fixed_rr'
        "target_mode": "opposite",      # 'opposite' = opposing CRT level, 'fixed_rr' = R:R based
        "session_only": True,           # restrict to RTH via data.get_session_mask
        "session_start": "09:30",       # NY session start (ET)
        "session_end": "16:00",         # RTH end (ET)
        "tz": "America/New_York",
        "tick_size": 0.25,
        "buffer_ticks": 1,              # stop is placed buffer_ticks outside the CRT level
        "direction": None,              # None = both, 1 = long only, -1 = short only
        "first_setup_per_session": True, # only first CRT setup each session
    }


def _simulate_trade(
    df_1m: pd.DataFrame,
    entry_time: pd.Timestamp,
    direction: int,
    entry_price: float,
    stop_loss: float,
    take_profit: float,
) -> tuple:
    """Walk forward on 1m bars and return the first stop or target exit.

    Stop-loss has priority when both levels are touched in the same bar.
    """
    future = df_1m.loc[df_1m.index > entry_time]
    if future.empty:
        # Degenerate case: close at the final bar.
        last = df_1m.iloc[-1]
        return entry_time, last["close"], "end_of_data", 0.0

    for ts, bar in future.iterrows():
        if direction == 1:  # long
            if bar["low"] <= stop_loss:
                return ts, stop_loss, "stop", (stop_loss - entry_price)
            if bar["high"] >= take_profit:
                return ts, take_profit, "target", (take_profit - entry_price)
        else:  # short
            if bar["high"] >= stop_loss:
                return ts, stop_loss, "stop", (entry_price - stop_loss)
            if bar["low"] <= take_profit:
                return ts, take_profit, "target", (entry_price - take_profit)

    # No exit triggered – close at the last available price.
    last = future.iloc[-1]
    last_pnl = (last["close"] - entry_price) * direction
    return last.name, last["close"], "end_of_data", last_pnl


def generate_signals(df_1m: pd.DataFrame, params: Dict[str, Any] | None = None) -> pd.DataFrame:
    """Generate Nitro CRT signals from 1-minute NQ data.

    Parameters
    ----------
    df_1m : pd.DataFrame
        1-minute OHLCV data with a DatetimeIndex.
    params : dict, optional
        Override for any key in ``default_params()``.

    Returns
    -------
    pd.DataFrame
        Columns: entry_time, direction, entry_price, stop_loss, take_profit,
        exit_time, exit_price, pnl, exit_reason.
    """
    p = default_params()
    if params:
        p.update(params)

    df = df_1m.copy()
    if df.empty:
        return _empty_signals()

    if p["session_only"]:
        mask = data.get_session_mask(df, tz=p["tz"])
        df = df[mask]

    if df.empty:
        return _empty_signals()

    # Higher-timeframe candles define the CRT levels.
    htf = data.resample_timeframe(df, p["htf_timeframe"])
    if len(htf) < 2:
        return _empty_signals()

    # Lower-timeframe bars used for the actual entry trigger.
    entry_tf = p["entry_timeframe"]
    entry_df = df if entry_tf == "1m" else data.resample_timeframe(df, entry_tf)

    tick_size = p["tick_size"]
    buffer = p["buffer_ticks"] * tick_size
    target_mode = p["target_mode"]
    rr = p["risk_reward"]
    allowed_dir = p["direction"]

    trades = []
    last_exit_time: pd.Timestamp | None = None
    last_trade_session_date: pd.Timestamp | None = None
    first_setup_per_session = p.get("first_setup_per_session", True)

    # Iterate higher-timeframe intervals. The *previous* htf candle's high/low
    # are the CRT levels for the current interval.
    prev_time = htf.index[0]
    for cur_time, prev_high, prev_low in zip(
        htf.index[1:], htf["high"].iloc[:-1].values, htf["low"].iloc[:-1].values
    ):
        if prev_high <= prev_low:
            prev_time = cur_time
            continue

        # Candidate entry bars inside this higher-timeframe interval.
        slice_entry = entry_df.loc[(entry_df.index > prev_time) & (entry_df.index <= cur_time)]
        if last_exit_time is not None:
            slice_entry = slice_entry.loc[slice_entry.index > last_exit_time]
        if slice_entry.empty:
            prev_time = cur_time
            continue

        # Enforce only the first setup of each RTH session if requested.
        if first_setup_per_session:
            session_date = slice_entry.index[0].tz_convert(p["tz"]).normalize() if slice_entry.index[0].tz else slice_entry.index[0].normalize()
            if last_trade_session_date is not None and session_date == last_trade_session_date:
                prev_time = cur_time
                continue

        # Build a chronological list of possible entries in this interval.
        entries = []

        if allowed_dir in (None, 1):
            # Long setup: sweep the previous htf low, then close back above it.
            swept_low = (slice_entry["low"] <= prev_low).cumsum().astype(bool)
            long_cond = swept_low & (slice_entry["close"] > prev_low)
            if long_cond.any():
                bar = slice_entry[long_cond].iloc[0]
                entries.append((bar.name, 1, bar["close"]))

        if allowed_dir in (None, -1):
            # Short setup: sweep the previous htf high, then close back below it.
            swept_high = (slice_entry["high"] >= prev_high).cumsum().astype(bool)
            short_cond = swept_high & (slice_entry["close"] < prev_high)
            if short_cond.any():
                bar = slice_entry[short_cond].iloc[0]
                entries.append((bar.name, -1, bar["close"]))

        if not entries:
            prev_time = cur_time
            continue

        # Choose the earliest entry if both directions triggered.
        entries.sort(key=lambda x: x[0])
        entry_time, direction, entry_price = entries[0]

        if direction == 1:
            stop_loss = prev_low - buffer
            if target_mode == "opposite":
                take_profit = prev_high
            else:
                take_profit = entry_price + rr * (entry_price - stop_loss)
            if not (take_profit > entry_price > stop_loss):
                prev_time = cur_time
                continue
        else:
            stop_loss = prev_high + buffer
            if target_mode == "opposite":
                take_profit = prev_low
            else:
                take_profit = entry_price - rr * (stop_loss - entry_price)
            if not (take_profit < entry_price < stop_loss):
                prev_time = cur_time
                continue

        exit_time, exit_price, exit_reason, pnl = _simulate_trade(
            df, entry_time, direction, entry_price, stop_loss, take_profit
        )

        trades.append({
            "entry_time": entry_time,
            "direction": direction,
            "entry_price": entry_price,
            "stop_loss": stop_loss,
            "take_profit": take_profit,
            "exit_time": exit_time,
            "exit_price": exit_price,
            "pnl": pnl,
            "exit_reason": exit_reason,
        })
        last_exit_time = exit_time
        if first_setup_per_session:
            last_trade_session_date = entry_time.tz_convert(p["tz"]).normalize() if entry_time.tz else entry_time.normalize()
        prev_time = cur_time

    return pd.DataFrame(trades)


def _empty_signals() -> pd.DataFrame:
    return pd.DataFrame(
        columns=[
            "entry_time", "direction", "entry_price", "stop_loss",
            "take_profit", "exit_time", "exit_price", "pnl", "exit_reason",
        ]
    )
