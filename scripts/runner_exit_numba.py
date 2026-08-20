#!/usr/bin/env python3
# CHANGE_SUMMARY
# 2026-08-19  kilo
#   - Fixed hold_day mode so it never holds past the end of the entry day.
#     When day_end_time is not present in the masked DataFrame (e.g. NYA
#     session ends at 12:00 but day_end is 16:00), the runner exits at the
#     last bar of the entry day with reason end_of_data instead of holding
#     across the entire backtest history.
#   - Added local_day array to the simulation cache and passed it into the
#     JIT kernel so calendar-day boundaries are visible to the runner.
# 2026-08-20  coder
#   - Added breakeven_then_trail runner mode (mode code 4).  After the
#     take-profit level is hit the stop is moved to the TP price, then
#     trailed by trail_atr_mult * ATR while the TP price acts as a floor
#     (long) or ceiling (short) for the trailing stop.
# WHY: The seconds-since-midnight time check could not distinguish days, so
#      a missing day_end caused trades to be carried for years.

"""Numba-accelerated runner exit simulation (per-trade JIT loop)."""
from __future__ import annotations

from typing import Any, Optional

import numpy as np
import pandas as pd

try:
    from numba import njit
except ImportError:  # pragma: no cover
    njit = None


def _time_to_seconds(t: Any) -> int:
    if t is None:
        return -1
    if isinstance(t, pd.Timestamp):
        t = t.time()
    return t.hour * 3600 + t.minute * 60 + t.second


def _simulate_arrays(df: pd.DataFrame) -> dict[str, np.ndarray]:
    """Cache NumPy views used by the runner exit logic."""
    cache = df.attrs.get("_simulate_arrays_numba")
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
        "local_day": (ns // 86_400_000_000_000).astype(np.int64),
    }
    df.attrs["_simulate_arrays_numba"] = cache
    return cache


def _ts_from_array(ts_arr: np.ndarray, idx: int) -> pd.Timestamp:
    return pd.Timestamp(ts_arr[idx]).tz_localize("UTC")


if njit is not None:
    @njit(cache=True)
    def _simulate_runner_numba_one(
        index: np.ndarray,
        high: np.ndarray,
        low: np.ndarray,
        close: np.ndarray,
        atr: np.ndarray,
        local_time_s: np.ndarray,
        local_day: np.ndarray,
        entry_idx: int,
        direction: int,
        stop_loss: float,
        take_profit: float,
        mode: int,  # 0=trail, 1=hold_day, 2=hold_session, 3=baseline, 4=breakeven_then_trail
        trail_mult: float,
        session_end_sec: int,
        day_end_sec: int,
        stop_first: bool,
    ) -> tuple[int, float, int]:
        n = index.shape[0]
        start = entry_idx + 1
        if start >= n:
            return n - 1, float(close[n - 1]), 5  # end_of_data

        d = direction
        sl = stop_loss
        tp = take_profit
        entry_day = local_day[entry_idx]

        # For hold_day, cap all scanning to the entry day so a missing
        # day_end_time cannot carry the trade across days or years.
        loop_end = n
        if mode == 1:
            last_entry_day_idx = n - 1
            for k in range(entry_idx, n):
                if local_day[k] != entry_day:
                    last_entry_day_idx = k - 1
                    break
            loop_end = last_entry_day_idx + 1
            if start >= loop_end:
                # Entry is on the last bar of the entry day; exit there.
                return entry_idx, float(close[entry_idx]), 5  # end_of_data

        for i in range(start, loop_end):
            # Baseline SL / TP checks.
            if d == 1:
                sl_hit = low[i] <= sl
                tp_hit = high[i] >= tp
            else:
                sl_hit = high[i] >= sl
                tp_hit = low[i] <= tp

            if sl_hit and not tp_hit:
                return i, sl, 0  # sl

            if tp_hit:
                # If SL also hit on the same bar and stop_first is True,
                # the runner is never activated.
                if sl_hit and stop_first:
                    return i, sl, 0  # sl

                # Baseline mode exits immediately at TP (no runner).
                if mode == 3:
                    return i, tp, 1  # tp

                # Runner phase starts on the next bar.
                runner_start = i + 1
                if runner_start >= loop_end:
                    # TP was hit on the last bar of the entry day; close at TP.
                    return i, tp, 1  # tp

                runner_stop = tp
                for j in range(runner_start, loop_end):
                    if mode == 2 and session_end_sec >= 0 and local_time_s[j] >= session_end_sec:
                        return j, float(close[j]), 2  # session_end
                    if mode == 1 and day_end_sec >= 0 and local_time_s[j] >= day_end_sec:
                        return j, float(close[j]), 3  # day_end
                    if mode == 0 or mode == 4:
                        if d == 1:
                            cand = high[j] - trail_mult * atr[j]
                            if mode == 4:
                                # breakeven_then_trail: never let the trailing
                                # stop fall back below the original take-profit.
                                if cand < tp:
                                    cand = tp
                            if cand > runner_stop:
                                runner_stop = cand
                            if low[j] <= runner_stop:
                                return j, float(runner_stop), 4  # trail_stop
                        else:
                            cand = low[j] + trail_mult * atr[j]
                            if mode == 4:
                                # breakeven_then_trail: never let the trailing
                                # stop rise back above the original take-profit.
                                if cand > tp:
                                    cand = tp
                            if cand < runner_stop:
                                runner_stop = cand
                            if high[j] >= runner_stop:
                                return j, float(runner_stop), 4  # trail_stop
                # Runner never exited within available data / entry day.
                return loop_end - 1, float(close[loop_end - 1]), 5  # end_of_data

            # Baseline session / day end (only if we haven't hit TP/SL).
            if session_end_sec >= 0 and local_time_s[i] >= session_end_sec:
                return i, float(close[i]), 2  # session_end
            if day_end_sec >= 0 and local_time_s[i] >= day_end_sec:
                return i, float(close[i]), 3  # day_end

        # Reached the end of the allowed scan range without an exit.
        if mode == 1:
            return loop_end - 1, float(close[loop_end - 1]), 5  # end_of_data
        return n - 1, float(close[n - 1]), 5  # end_of_data

    @njit(cache=True)
    def _simulate_runner_numba_batch(
        index: np.ndarray,
        high: np.ndarray,
        low: np.ndarray,
        close: np.ndarray,
        atr: np.ndarray,
        local_time_s: np.ndarray,
        local_day: np.ndarray,
        entry_idx: np.ndarray,
        direction: np.ndarray,
        stop_loss: np.ndarray,
        take_profit: np.ndarray,
        mode: int,
        trail_mult: float,
        session_end_sec: int,
        day_end_sec: int,
        stop_first: bool,
    ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """Batch version of _simulate_runner_numba_one using parallel prange."""
        n = entry_idx.shape[0]
        exit_idx = np.empty(n, dtype=np.int64)
        exit_price = np.empty(n, dtype=np.float64)
        exit_reason = np.empty(n, dtype=np.int64)
        for t in range(n):
            eidx, eprice, ereason = _simulate_runner_numba_one(
                index,
                high,
                low,
                close,
                atr,
                local_time_s,
                local_day,
                int(entry_idx[t]),
                int(direction[t]),
                float(stop_loss[t]),
                float(take_profit[t]),
                mode,
                trail_mult,
                session_end_sec,
                day_end_sec,
                stop_first,
            )
            exit_idx[t] = eidx
            exit_price[t] = eprice
            exit_reason[t] = ereason
        return exit_idx, exit_price, exit_reason
else:
    def _simulate_runner_numba_one(*args, **kwargs):  # pragma: no cover
        raise RuntimeError("numba is required for the accelerated runner")

    def _simulate_runner_numba_batch(*args, **kwargs):  # pragma: no cover
        raise RuntimeError("numba is required for the accelerated runner")


_REASON_MAP = {
    0: "sl",
    1: "tp",
    2: "session_end",
    3: "day_end",
    4: "trail_stop",
    5: "end_of_data",
}


def apply_runner_to_signals(
    df: pd.DataFrame,
    signals: pd.DataFrame,
    mode: str = "trail",
    trail_atr_mult: float = 1.0,
    session_end_time: Optional[Any] = None,
    day_end_time: Optional[Any] = None,
) -> pd.DataFrame:
    """Re-simulate exits for a signal table using a runner rule (Numba version).

    ``df`` must contain an ``atr`` column and have a UTC DatetimeIndex.
    ``signals`` must contain entry_time, direction, entry_price, stop_loss,
    take_profit.
    """
    if signals.empty:
        return signals.copy()

    ar = _simulate_arrays(df)
    index = ar["index"]
    ts = pd.to_datetime(signals["entry_time"])
    if ts.dt.tz is None:
        ts = ts.dt.tz_localize("UTC")
    else:
        ts = ts.dt.tz_convert("UTC")
    ts = ts.values
    entry_idx = np.searchsorted(index, ts)

    mode_code = {"trail": 0, "hold_day": 1, "hold_session": 2, "baseline": 3, "breakeven_then_trail": 4}[mode]
    session_end_sec = _time_to_seconds(session_end_time)
    day_end_sec = _time_to_seconds(day_end_time)

    # Batch-process all signals in one Numba call to avoid per-trade Python
    # loop overhead.  This is the hot path for the comprehensive sweep.
    exit_idx, exit_prices, exit_reasons = _simulate_runner_numba_batch(
        index,
        ar["high"],
        ar["low"],
        ar["close"],
        ar["atr"],
        ar["local_time_s"],
        ar["local_day"],
        entry_idx,
        signals["direction"].values.astype(np.int64),
        signals["stop_loss"].values.astype(np.float64),
        signals["take_profit"].values.astype(np.float64),
        mode_code,
        float(trail_atr_mult),
        int(session_end_sec),
        int(day_end_sec),
        True,
    )

    exit_times = index[exit_idx]
    pnls = signals["direction"].values * (exit_prices - signals["entry_price"].values)

    trades = signals.copy()
    trades["exit_time"] = pd.to_datetime(exit_times, utc=True)
    trades["exit_price"] = exit_prices
    trades["exit_reason"] = np.vectorize(_REASON_MAP.get)(exit_reasons)
    trades["pnl"] = pnls
    return trades
