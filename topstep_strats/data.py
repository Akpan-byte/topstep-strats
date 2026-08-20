# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created topstep_strats/data.py implementing shared data interfaces.
#   - load_nq_data uses pyarrow engine for fast CSV parsing of large NQ 1m data.
#   - resample_timeframe aggregates OHLCV with pandas resample rules.
#   - split_by_date filters inclusive date ranges from string or datetime inputs.
#   - get_session_mask converts UTC index to NY time and masks RTH session bars.
# 2026-08-19  kilo
#   - Added load_instrument_data(instrument, data_dir) for NQ/ES/YM 1m parquet.
#   - load_market_data now supports both parquet and CSV inputs.
#   - load_nq_data defaults to data/NQ_1min.parquet, falling back to CSV.
# WHY: The data-v2.0 release ships ES/NQ/YM 1-minute bars as parquet files;
#      strategies and backtests need a single helper that loads any instrument.

from __future__ import annotations

from pathlib import Path
from typing import Union

import pandas as pd

DateLike = Union[str, pd.Timestamp]

# Valid instruments shipped in the data-v2.0 release.
INSTRUMENTS = {"NQ", "ES", "YM"}


def _normalize_columns(df: pd.DataFrame) -> pd.DataFrame:
    """Lower-case columns and ensure the required OHLCV set is present."""
    df = df.copy()
    df.columns = [c.lower() for c in df.columns]
    required = {"open", "high", "low", "close", "volume"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {missing}")
    return df[df.columns.intersection(required)].copy()


def _load_csv(path: Path) -> pd.DataFrame:
    """Load a 1-minute futures CSV into a UTC-indexed DataFrame."""
    # PyArrow engine parses large CSVs significantly faster than the C engine.
    df = pd.read_csv(
        path,
        engine="pyarrow",
        dtype={"open": "float64", "high": "float64", "low": "float64", "close": "float64", "volume": "int64"},
    )

    if "timestamp" not in df.columns:
        raise ValueError("CSV must contain a 'timestamp' column")

    df["timestamp"] = pd.to_datetime(df["timestamp"], utc=True)
    return df.set_index("timestamp").sort_index()


def _load_parquet(path: Path) -> pd.DataFrame:
    """Load a 1-minute futures parquet file into a UTC-indexed DataFrame."""
    df = pd.read_parquet(path)

    # Parquet files may store the timestamp as a column or as the index.
    if "timestamp" in df.columns:
        df["timestamp"] = pd.to_datetime(df["timestamp"], utc=True)
        df = df.set_index("timestamp")

    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    else:
        df.index = df.index.tz_convert("UTC")

    return df.sort_index()


def load_market_data(path: str) -> pd.DataFrame:
    """Load a 1-minute futures CSV or parquet into a UTC-indexed DataFrame.

    Parameters
    ----------
    path:
        Path to CSV or parquet with columns timestamp,open,high,low,close,volume.

    Returns
    -------
    DataFrame indexed by ``timestamp`` (UTC datetime64) with lowercase
    columns open/high/low/close/volume.
    """
    data_path = Path(path)
    if not data_path.exists():
        raise FileNotFoundError(f"Market data not found at {path}")

    suffix = data_path.suffix.lower()
    if suffix == ".parquet":
        df = _load_parquet(data_path)
    elif suffix == ".csv":
        df = _load_csv(data_path)
    else:
        raise ValueError(f"Unsupported file extension: {suffix!r} (expected .csv or .parquet)")

    return _normalize_columns(df)


def load_nq_data(path: str | None = None) -> pd.DataFrame:
    """Backward-compatible alias for load_market_data.

    Defaults to ``data/NQ_1min.parquet`` (data-v2.0 release) when no path is
    supplied, falling back to the legacy CSV locations for compatibility.
    """
    if path is not None:
        return load_market_data(path)

    project_root = Path(__file__).resolve().parent.parent
    candidates = [
        project_root / "data" / "NQ_1min.parquet",
        Path("/tmp/market_data/NQ_1min.parquet"),
        project_root / "data" / "NQ_1min.csv",
        Path("/tmp/market_data/NQ_1min.csv"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return load_market_data(str(candidate))

    raise FileNotFoundError("NQ 1-minute data not found in data/ or /tmp/market_data/")


def load_instrument_data(instrument: str, data_dir: str) -> pd.DataFrame:
    """Load 1-minute OHLCV data for a supported futures instrument.

    Parameters
    ----------
    instrument:
        One of ``NQ``, ``ES``, or ``YM`` (case-insensitive).
    data_dir:
        Directory containing ``{instrument}_1min.parquet`` from the data-v2.0
        release.

    Returns
    -------
    DataFrame indexed by UTC timestamp with lowercase open/high/low/close/volume.
    """
    symbol = instrument.upper()
    if symbol not in INSTRUMENTS:
        raise ValueError(f"Unsupported instrument: {instrument!r}; expected one of {sorted(INSTRUMENTS)}")

    path = Path(data_dir) / f"{symbol}_1min.parquet"
    if not path.exists():
        raise FileNotFoundError(f"Instrument data not found at {path}")

    return load_market_data(str(path))


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

    # Keep only columns we know how to aggregate.  Drop any attrs cached by
    # Numba/array helpers so pandas concat's __finalize__ does not try to
    # compare numpy arrays (which raises "ambiguous array" ValueError).
    cols = [c for c in agg if c in df.columns]
    clean = df[cols].copy()
    clean.attrs = {}
    resampled = clean.resample(tf, label="left", closed="left").agg({c: agg[c] for c in cols})
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
