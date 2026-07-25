# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created topstep_strats/data.py implementing shared data interfaces.
#   - load_nq_data uses pyarrow engine for fast CSV parsing of large NQ 1m data.
#   - resample_timeframe aggregates OHLCV with pandas resample rules.
#   - split_by_date filters inclusive date ranges from string or datetime inputs.
#   - get_session_mask converts UTC index to NY time and masks RTH session bars.
# WHY: Provide a fast, consistent data layer for all strategy/backtest agents.

from __future__ import annotations

from pathlib import Path
from typing import Union

import pandas as pd

DateLike = Union[str, pd.Timestamp]


def load_market_data(csv_path: str) -> pd.DataFrame:
    """Load a 1-minute futures CSV into a UTC-indexed DataFrame.

    Parameters
    ----------
    csv_path:
        Path to CSV with columns timestamp,open,high,low,close,volume.

    Returns
    -------
    DataFrame indexed by ``timestamp`` (UTC datetime64) with lowercase
    columns open/high/low/close/volume.
    """
    path = Path(csv_path)
    if not path.exists():
        raise FileNotFoundError(f"Market data not found at {csv_path}")

    # PyArrow engine parses large CSVs significantly faster than the C engine.
    df = pd.read_csv(
        path,
        engine="pyarrow",
        dtype={"open": "float64", "high": "float64", "low": "float64", "close": "float64", "volume": "int64"},
    )

    if "timestamp" not in df.columns:
        raise ValueError("CSV must contain a 'timestamp' column")

    df["timestamp"] = pd.to_datetime(df["timestamp"], utc=True)
    df = df.set_index("timestamp").sort_index()
    df.columns = [c.lower() for c in df.columns]

    required = {"open", "high", "low", "close", "volume"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    return df[df.columns.intersection(required)].copy()


def load_nq_data(csv_path: str = "/tmp/market_data/NQ_1min.csv") -> pd.DataFrame:
    """Backward-compatible alias for load_market_data."""
    return load_market_data(csv_path)


def _normalize_timeframe(timeframe: str) -> str:
    """Convert common shorthand (e.g. '15m') to pandas 2.x offsets ('15min')."""
    tf = timeframe.strip().lower()
    if tf.endswith("m") and not tf.endswith("min"):
        return tf[:-1] + "min"
    return timeframe


def resample_timeframe(df: pd.DataFrame, timeframe: str = "15m") -> pd.DataFrame:
    """Resample 1m OHLCV data to a higher timeframe.

    Parameters
    ----------
    df:
        Input DataFrame with open/high/low/close/volume columns.
    timeframe:
        Pandas offset string (e.g. ``'5m'``, ``'15m'``, ``'1h'``).

    Returns
    -------
    Resampled DataFrame with open/high/low/close/volume columns.
    """
    if df.empty:
        return df.copy()

    tf = _normalize_timeframe(timeframe)

    agg = {
        "open": "first",
        "high": "max",
        "low": "min",
        "close": "last",
        "volume": "sum",
    }

    # Keep only columns we know how to aggregate.
    cols = [c for c in agg if c in df.columns]
    resampled = df[cols].resample(tf, label="left", closed="left").agg({c: agg[c] for c in cols})
    return resampled.dropna()


def split_by_date(df: pd.DataFrame, start: DateLike, end: DateLike) -> pd.DataFrame:
    """Filter DataFrame to an inclusive date range.

    Parameters
    ----------
    df:
        Input DataFrame with a DatetimeIndex.
    start, end:
        Inclusive boundary dates. Strings are parsed; times are ignored and
        the full calendar day is included.
    """
    start_ts = pd.Timestamp(start, tz=df.index.tz) if df.index.tz else pd.Timestamp(start)
    end_ts = pd.Timestamp(end, tz=df.index.tz) if df.index.tz else pd.Timestamp(end)

    # Normalize to start/end of day so the full calendar day is included.
    start_day = start_ts.normalize()
    end_day = end_ts.normalize() + pd.Timedelta(days=1) - pd.Timedelta(nanoseconds=1)

    return df.loc[start_day:end_day].copy()


def get_session_mask(
    df: pd.DataFrame,
    start_time: str = "09:30",
    end_time: str = "16:00",
    tz: str = "America/New_York",
) -> pd.Series:
    """Return a boolean Series masking bars inside the target session.

    Parameters
    ----------
    df:
        Input DataFrame with a UTC DatetimeIndex.
    start_time:
        Session start local time (HH:MM).
    end_time:
        Session end local time (HH:MM).
    tz:
        Timezone used to interpret start/end times.

    Returns
    -------
    Boolean Series aligned to ``df.index``.
    """
    if df.empty:
        return pd.Series(dtype=bool)

    local = df.index.tz_convert(tz)
    start = pd.Timestamp(start_time).time()
    end = pd.Timestamp(end_time).time()

    times = local.time
    if start < end:
        values = (times >= start) & (times <= end)
    else:
        # Overnight session handling (e.g. 18:00-09:30).
        values = (times >= start) | (times <= end)

    return pd.Series(values, index=df.index, dtype=bool)
