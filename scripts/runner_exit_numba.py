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
# 2026-08-27  kilo
#   - Added breakeven_then_trail mode: once TP is hit, stop moves to entry
#     price, then trails by trail_mult*ATR.
#   - Added baseline mode mapping so signals exit at TP/SL/session end with
#     no runner.
#   - Passed entry_price into the JIT kernel so the breakeven floor can be
#     enforced exactly.
# WHY: The 150k standard sweep was crashing because the pre-filter selected
#      breakeven_0.5x / breakeven_1.0x candidates, which were not supported
#      by the runner mapping. The new modes unblock those high-payout configs.

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
        entry_price: float,
        stop_loss: float,
        take_profit: float,
        mode: int,  # 0=trail, 1=hold_day, 2=hold_session, 3=breakeven_then_trail, 4=baseline
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

                # Baseline mode: take profit and exit immediately.
                if mode == 4:
                    return i, tp, 1  # tp

                # Runner phase starts on the next bar.
                runner_start = i + 1
                if runner_start >= loop_end:
                    # TP was hit on the last bar of the entry day; close at TP.
                    return i, tp, 1  # tp

                # Breakeven floor: once TP is touched, stop cannot move
                # back past the entry price. Then trail by trail_mult*ATR.
                if mode == 3:
                    runner_stop = entry_price
                else:
                    runner_stop = tp

                for j in range(runner_start, loop_end):
                    if mode in (2, 3) and local_time_s[j] >= session_end_sec:
                        return j, float(close[j]), 2  # session_end
                    if mode == 1 and local_time_s[j] >= day_end_sec:
                        return j, float(close[j]), 3  # day_end
                    if mode in (0, 3):
                        if d == 1:
                            cand = high[j] - trail_mult * atr[j]
                            if cand > runner_stop:
                                runner_stop = cand
                            if low[j] <= runner_stop:
                                return j, float(runner_stop), 4  # trail_stop
                        else:
                            cand = low[j] + trail_mult * atr[j]
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
else:
    def _simulate_runner_numba_one(*args, **kwargs):  # pragma: no cover
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

    mode_code = {
        "trail": 0,
        "hold_day": 1,
        "hold_session": 2,
        "breakeven_then_trail": 3,
        "baseline": 4,
    }[mode]
    session_end_sec = _time_to_seconds(session_end_time)
    day_end_sec = _time_to_seconds(day_end_time)

    n = len(signals)
    exit_times = np.empty(n, dtype="datetime64[ns]")
    exit_prices = np.empty(n, dtype=np.float64)
    exit_reasons = np.empty(n, dtype=object)
    pnls = np.empty(n, dtype=np.float64)

    # Pre-bind arrays for the JIT call (avoids attribute lookups in the loop).
    idx_arr = index
    high_arr = ar["high"]
    low_arr = ar["low"]
    close_arr = ar["close"]
    atr_arr = ar["atr"]
    lt_arr = ar["local_time_s"]
    ld_arr = ar["local_day"]

    for t in range(n):
        ei = entry_idx[t]
        eidx, eprice, ereason = _simulate_runner_numba_one(
            idx_arr,
            high_arr,
            low_arr,
            close_arr,
            atr_arr,
            lt_arr,
            ld_arr,
            int(ei),
            int(signals["direction"].iat[t]),
            float(signals["entry_price"].iat[t]),
            float(signals["stop_loss"].iat[t]),
            float(signals["take_profit"].iat[t]),
            mode_code,
            float(trail_atr_mult),
            int(session_end_sec),
            int(day_end_sec),
            True,
        )
        exit_times[t] = idx_arr[eidx]
        exit_prices[t] = eprice
        exit_reasons[t] = _REASON_MAP[int(ereason)]
        pnls[t] = float(signals["direction"].iat[t]) * (eprice - float(signals["entry_price"].iat[t]))

    trades = signals.copy()
    trades["exit_time"] = pd.to_datetime(exit_times, utc=True)
    trades["exit_price"] = exit_prices
    trades["exit_reason"] = exit_reasons
    trades["pnl"] = pnls
    return trades
