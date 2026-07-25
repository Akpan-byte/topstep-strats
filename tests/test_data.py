# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created tests/test_data.py verifying shared data interfaces.
#   - Tests cover loader column/index shape, session mask boundaries,
#     resampling OHLCV aggregation, and inclusive date splitting.
# WHY: Ensure the data layer behaves correctly before strategy agents consume it.

from __future__ import annotations

import pandas as pd
import pytest

from topstep_strats.data import get_session_mask, load_nq_data, resample_timeframe, split_by_date


@pytest.fixture
def sample_df() -> pd.DataFrame:
    """Small synthetic 1m DataFrame spanning two NY session days."""
    # 2016-06-01 was a Wednesday. Use explicit UTC timestamps.
    idx = pd.date_range("2016-06-01 00:00:00", periods=60, freq="1min", tz="UTC")
    df = pd.DataFrame(
        {
            "open": 4500.0,
            "high": 4501.0,
            "low": 4499.0,
            "close": 4500.5,
            "volume": 10,
        },
        index=idx,
    )
    return df


def test_load_nq_data_columns_and_index(tmp_path):
    csv = tmp_path / "nq.csv"
    csv.write_text(
        "timestamp,open,high,low,close,volume\n"
        "2016-06-01 09:30:00,4500.0,4501.0,4499.0,4500.5,100\n"
        "2016-06-01 09:31:00,4500.5,4502.0,4500.0,4501.5,200\n"
    )
    df = load_nq_data(str(csv))

    assert list(df.columns) == ["open", "high", "low", "close", "volume"]
    assert df.index.name == "timestamp"
    assert df.index.tz is not None
    assert len(df) == 2


def test_load_nq_data_missing_file():
    with pytest.raises(FileNotFoundError):
        load_nq_data("/tmp/nonexistent_nq.csv")


def test_get_session_mask_rth(sample_df):
    mask = get_session_mask(sample_df, start_time="09:30", end_time="16:00")

    # 2016-06-01 00:00 UTC == 20:00 ET (previous day). No bars in RTH.
    assert mask.sum() == 0

    # June 1 2016 is EDT (UTC-4). 13:30 UTC == 09:30 ET.
    shifted = sample_df.copy()
    shifted.index = shifted.index + pd.Timedelta(hours=13, minutes=30)
    mask2 = get_session_mask(shifted, start_time="09:30", end_time="16:00")
    assert mask2.sum() > 0
    assert mask2.iloc[0]


def test_resample_timeframe_ohlcv(sample_df):
    df15 = resample_timeframe(sample_df, timeframe="15m")
    assert set(df15.columns) == {"open", "high", "low", "close", "volume"}
    # 60 1m bars -> 4 15m bars.
    assert len(df15) == 4
    # Volume should sum across constituent bars.
    assert (df15["volume"] == 150).all()
    assert df15["high"].max() == 4501.0
    assert df15["low"].min() == 4499.0


def test_split_by_date_inclusive(sample_df):
    # sample_df spans 2016-06-01 only; shift to cover two days.
    df = sample_df.copy()
    df.index = pd.date_range("2016-06-01 12:00:00", periods=60, freq="1min", tz="UTC")

    subset = split_by_date(df, start="2016-06-01", end="2016-06-01")
    assert subset.index.date.min() == pd.Timestamp("2016-06-01").date()
    assert subset.index.date.max() == pd.Timestamp("2016-06-01").date()

    subset2 = split_by_date(df, start="2016-06-01", end="2016-06-02")
    assert len(subset2) == len(df)


def test_empty_dataframe_handling():
    empty = pd.DataFrame(columns=["open", "high", "low", "close", "volume"]).astype(
        {"open": float, "high": float, "low": float, "close": float, "volume": int}
    )
    empty.index = pd.DatetimeIndex([], tz="UTC", name="timestamp")

    assert resample_timeframe(empty).empty
    assert split_by_date(empty, start="2016-06-01", end="2016-06-01").empty
    assert get_session_mask(empty).empty
