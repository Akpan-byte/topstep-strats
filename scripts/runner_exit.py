#!/usr/bin/env python3
"""Runner exit variants for Paper-1/2 strategies.

After the original take-profit level is touched, move the stop-loss to the
original TP (breakeven on the runner) and then either:

- trail: trail the stop by ``trail_atr_mult * ATR(14)`` below/above the
  extreme until the trailing stop is hit.
- hold_day: hold until the NY day close (16:00 EST) using the close price.
- hold_session: hold until the configured session end time using the close.

The original stop-loss is still in force *before* the TP is touched, so trades
that never reach TP behave exactly like the standard simulation.
"""
from __future__ import annotations

from typing import Any, Optional, Tuple

import numpy as np
import pandas as pd


def _simulate_arrays(df: pd.DataFrame) -> dict[str, np.ndarray]:
    """Cache NumPy views used by the runner exit logic."""
    cache = df.attrs.get("_simulate_arrays")
    if cache is not None:
        return cache
    local = df.index.tz_convert("America/New_York").tz_localize(None)
    ns = local.astype("datetime64[ns]").view(np.int64)
    cache = {
        "index": df.index.values,
        "high": df["high"].values,
        "low": df["low"].values,
        "close": df["close"].values,
        "atr": df["atr"].values if "atr" in df.columns else np.zeros(len(df)),
        "local_time_s": (ns % 86_400_000_000_000) // 1_000_000_000,
    }
    df.attrs["_simulate_arrays"] = cache
    return cache


def _ts_from_array(ts_arr: np.ndarray, idx: int) -> pd.Timestamp:
    return pd.Timestamp(ts_arr[idx]).tz_localize("UTC")


def simulate_runner_exit(
    df: pd.DataFrame,
    entry_time: pd.Timestamp,
    direction: int,
    entry_price: float,
    stop_loss: float,
    take_profit: float,
    mode: str = "trail",
    trail_atr_mult: float = 1.0,
    session_end_time: Optional[Any] = None,
    day_end_time: Optional[Any] = None,
    stop_first: bool = True,
) -> Tuple[pd.Timestamp, float, str, float]:
    """Return the first exit for a runner-style trade.

    Parameters
    ----------
    mode :
        ``"trail"`` uses an ATR-based trailing stop after TP.
        ``"hold_day"`` holds until ``day_end_time``.
        ``"hold_session"`` holds until ``session_end_time``.
    """
    ar = _simulate_arrays(df)
    ts = ar["index"]
    high = ar["high"]
    low = ar["low"]
    close = ar["close"]
    atr = ar["atr"]
    local_time_s = ar["local_time_s"]
    n = len(ts)

    entry_ts = entry_time.tz_convert("UTC").asm8
    pos = int(np.searchsorted(ts, entry_ts))
    start = pos + 1
    if start >= n:
        lc = float(close[-1])
        return df.index[-1], lc, "end_of_data", direction * (lc - entry_price)

    fut_ts = ts[start:]
    fut_high = high[start:]
    fut_low = low[start:]
    fut_close = close[start:]
    fut_atr = atr[start:]
    fut_lt = local_time_s[start:]

    if direction == 1:
        sl_idx = np.where(fut_low <= stop_loss)[0]
        tp_idx = np.where(fut_high >= take_profit)[0]
    else:
        sl_idx = np.where(fut_high >= stop_loss)[0]
        tp_idx = np.where(fut_low <= take_profit)[0]

    sl_first = int(sl_idx[0]) if sl_idx.size else None
    tp_first = int(tp_idx[0]) if tp_idx.size else None

    # If TP never hits, use the standard first-exit logic.
    if tp_first is None:
        best_idx, best_price, best_reason = sl_first, stop_loss, "sl"
        if sl_first is None:
            best_idx, best_price, best_reason = None, None, None
        # Session end baseline.
        if session_end_time is not None:
            sess_sec = session_end_time.hour * 3600 + session_end_time.minute * 60 + session_end_time.second
            sess_idx = np.where(fut_lt >= sess_sec)[0]
            if sess_idx.size:
                si = int(sess_idx[0])
                if best_idx is None or si < best_idx:
                    best_idx = si
                    best_price = float(fut_close[si])
                    best_reason = "session_end"
        if best_idx is None:
            lc = float(fut_close[-1])
            return _ts_from_array(fut_ts, -1), lc, "end_of_data", direction * (lc - entry_price)
        return _ts_from_array(fut_ts, best_idx), float(best_price), best_reason, direction * (float(best_price) - entry_price)

    # TP was hit before we consider the runner.  If SL also hit on the same bar
    # and stop_first is True, the runner is never activated.
    if sl_first is not None and sl_first < tp_first:
        return _ts_from_array(fut_ts, sl_first), stop_loss, "sl", direction * (stop_loss - entry_price)
    if sl_first == tp_first and stop_first:
        return _ts_from_array(fut_ts, sl_first), stop_loss, "sl", direction * (stop_loss - entry_price)

    # Runner phase: move stop to TP (breakeven) and continue from the bar
    # after the TP bar.
    runner_start = tp_first + 1
    if runner_start >= len(fut_ts):
        # TP was hit on the last available bar; close at TP.
        return _ts_from_array(fut_ts, tp_first), take_profit, "tp", direction * (take_profit - entry_price)

    stop = take_profit
    exit_idx = None
    exit_price = None
    exit_reason = None

    for i in range(runner_start, len(fut_ts)):
        # Session/day-end checks take priority over the trailing stop if they
        # come first on this bar.
        if session_end_time is not None and mode == "hold_session":
            sess_sec = session_end_time.hour * 3600 + session_end_time.minute * 60 + session_end_time.second
            if fut_lt[i] >= sess_sec:
                exit_idx, exit_price, exit_reason = i, float(fut_close[i]), "session_end"
                break

        if day_end_time is not None and mode == "hold_day":
            day_sec = day_end_time.hour * 3600 + day_end_time.minute * 60 + day_end_time.second
            if fut_lt[i] >= day_sec:
                exit_idx, exit_price, exit_reason = i, float(fut_close[i]), "day_end"
                break

        if mode == "trail":
            if direction == 1:
                cand = fut_high[i] - trail_atr_mult * fut_atr[i]
                if cand > stop:
                    stop = cand
                if fut_low[i] <= stop:
                    exit_idx, exit_price, exit_reason = i, float(stop), "trail_stop"
                    break
            else:
                cand = fut_low[i] + trail_atr_mult * fut_atr[i]
                if cand < stop:
                    stop = cand
                if fut_high[i] >= stop:
                    exit_idx, exit_price, exit_reason = i, float(stop), "trail_stop"
                    break

    if exit_idx is None:
        lc = float(fut_close[-1])
        return _ts_from_array(fut_ts, -1), lc, "end_of_data", direction * (lc - entry_price)

    return _ts_from_array(fut_ts, exit_idx), exit_price, exit_reason, direction * (exit_price - entry_price)


def apply_runner_to_signals(
    df: pd.DataFrame,
    signals: pd.DataFrame,
    mode: str = "trail",
    trail_atr_mult: float = 1.0,
    session_end_time: Optional[Any] = None,
    day_end_time: Optional[Any] = None,
) -> pd.DataFrame:
    """Re-simulate exits for a signal table using a runner rule.

    ``df`` must contain an ``atr`` column (e.g. ATR(14) shifted by 1 bar) and
    have a UTC DatetimeIndex.  ``signals`` must contain entry_time, direction,
    entry_price, stop_loss, take_profit.
    """
    if signals.empty:
        return signals.copy()

    trades = signals.copy()
    results = []
    for _, row in trades.iterrows():
        et = row["entry_time"]
        direction = int(row["direction"])
        entry_price = float(row["entry_price"])
        sl = float(row["stop_loss"])
        tp = float(row["take_profit"])
        et_out, ep_out, reason, pnl = simulate_runner_exit(
            df,
            et,
            direction,
            entry_price,
            sl,
            tp,
            mode=mode,
            trail_atr_mult=trail_atr_mult,
            session_end_time=session_end_time,
            day_end_time=day_end_time,
            stop_first=True,
        )
        results.append(
            {
                "entry_time": et,
                "direction": direction,
                "entry_price": entry_price,
                "stop_loss": sl,
                "take_profit": tp,
                "exit_time": et_out,
                "exit_price": ep_out,
                "pnl": pnl,
                "exit_reason": reason,
            }
        )

    return pd.DataFrame(results)
