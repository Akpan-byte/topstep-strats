# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Fixed _casper_inverted_fvg to evaluate the gap that existed at the
#     previous bar instead of the gap at the current bar.  _fvg_detect
#     invalidates a gap as soon as price touches it, so checking the prior
#     bar's gap while using the current bar's prices for fill/confirmation
#     lets the inverted-FVG entry actually trigger.
#   - Fixed _wade_pats_second_entry to record the swing high/low level from
#     the peak bar (i-1) rather than the confirmation bar (i).  _swing_highs_lows
#     shifts the marker by one bar, so the previous bar holds the actual level.
# 2026-08-19  kilo
#   - Implemented topstep_strats/strategies/paper1_matrix.py with the 7 Paper-1
#     blueprints: ICT Silver Bullet, Casper SMC Inverted FVG, Velez 20/200
#     Elephant Bar, Rosato S/D Absorption, Carter TTM Squeeze, Raschke Holy
#     Grail, and Wade PATs Second Entry.
#   - Added vectorized indicator helpers (EMA, SMA, ATR, Bollinger Bands,
#     Keltner Channels, ADX, RSI, MACD, VWAP, volume SMA, FVG detection,
#     swing highs/lows) that only use closed-bar data (shift(1)) to avoid
#     lookahead bias.
#   - Built a deterministic 100-row Paper-1 config matrix (IDs 001-100). Each
#     row maps to a blueprint + instrument + session + filter + TP/SL ATR
#     multipliers.  get_strategy_config(id) returns the params dict expected by
#     generate_signals.
#   - generate_signals(df_1m, params) dispatches by blueprint and returns a
#     DataFrame with the canonical columns:
#       entry_time, direction, entry_price, stop_loss, take_profit,
#       exit_time, exit_price, pnl, exit_reason.
# WHY: Provides the Paper-1 strategy leg for the TopStep parallel backtest
#      project on the matrix-sweep-paper1 branch.
# NOTE: The exact Paper-1 parameter table was not present in the repository, so
#       the 100-row matrix below is a canonical parameter sweep across the 7
#       blueprints.  Replace _MATRIX rows if a specific table is provided.

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd

from topstep_strats import data


# -----------------------------------------------------------------------------
# Public matrix metadata
# -----------------------------------------------------------------------------

BLUEPRINTS = [
    "ict_silver_bullet",
    "casper_inverted_fvg",
    "velez_20_200_elephant_bar",
    "rosato_sd_absorption",
    "carter_ttm_squeeze",
    "raschke_holy_grail",
    "wade_pats_second_entry",
]


# -----------------------------------------------------------------------------
# Default parameters shared by every Paper-1 config
# -----------------------------------------------------------------------------

def default_params() -> Dict[str, Any]:
    """Defaults common to all Paper-1 blueprints."""
    return {
        "blueprint": "ict_silver_bullet",
        "instrument": "NQ",
        "tick_size": 0.25,
        "point_value": 20.0,
        "session": "NY",
        "session_start": "09:30",
        "session_end": "16:00",
        "tz": "America/New_York",
        "session_only": True,
        "one_trade_per_day": True,
        "filter": None,
        "tp_atr": 2.0,
        "sl_atr": 5.0,
        "atr_length": 14,
        "stop_first": True,
    }


# -----------------------------------------------------------------------------
# Session definitions
# -----------------------------------------------------------------------------

SESSIONS: Dict[str, Dict[str, str]] = {
    "ASN": {"start": "20:00", "end": "23:00", "tz": "America/New_York"},
    "LON": {"start": "03:00", "end": "11:00", "tz": "America/New_York"},
    "NYA": {"start": "09:30", "end": "12:00", "tz": "America/New_York"},
    "NYP": {"start": "12:00", "end": "16:00", "tz": "America/New_York"},
    "NY":  {"start": "09:30", "end": "16:00", "tz": "America/New_York"},
    "Asian": {"start": "20:00", "end": "23:00", "tz": "America/New_York"},
    "London": {"start": "03:00", "end": "11:00", "tz": "America/New_York"},
}


# -----------------------------------------------------------------------------
# 100-row Paper-1 matrix
#
# NOTE: The exact Paper-1 table was not available in the repo.  The matrix below
# is a structured parameter sweep over the 7 blueprints.  Each row carries an
# instrument, session, blueprint, filter expression, and TP/SL ATR multipliers.
# -----------------------------------------------------------------------------

def _build_matrix() -> List[Dict[str, Any]]:
    """Return the exact Paper-1 100-row strategy matrix from the research report.

    Each row carries the instrument, session, blueprint, filter expression, and
    TP/SL ATR multipliers specified by the user.  IDs are assigned sequentially
    from 001 to 100.  The backtest runners override the stored instrument and
    session at runtime so every strategy can be evaluated across NQ/ES/YM and
    Asian/London/NY sessions.
    """
    rows: List[Dict[str, Any]] = []

    _raw = [
        ("001", "NQ", "NYA", "ict_silver_bullet", "ema20_gt_ema50", 0.10),
        ("002", "ES", "NYP", "ict_silver_bullet", "price_gt_vwap", 0.10),
        ("003", "YM", "LON", "ict_silver_bullet", "adx_gt_25", 0.15),
        ("004", "NQ", "ASN", "ict_silver_bullet", "rsi_lt_70", 0.05),
        ("005", "ES", "LON", "ict_silver_bullet", "volume_gt_sma20", 0.10),
        ("006", "YM", "NYA", "ict_silver_bullet", "macd_hist_gt_0", 0.20),
        ("007", "NQ", "NYP", "casper_inverted_fvg", "price_lt_vwap", 0.10),
        ("008", "ES", "LON", "casper_inverted_fvg", "ema9_lt_ema21", 0.15),
        ("009", "YM", "ASN", "casper_inverted_fvg", "adx_gt_30", 0.05),
        ("010", "NQ", "NYA", "casper_inverted_fvg", "rsi_gt_30", 0.10),
        ("011", "ES", "NYP", "casper_inverted_fvg", "volume_gt_sma50", 0.10),
        ("012", "YM", "LON", "casper_inverted_fvg", "price_gt_sma200", 0.20),
        ("013", "NQ", "LON", "velez_20_200_elephant_bar", "adx_gt_20", 0.15),
        ("014", "ES", "NYA", "velez_20_200_elephant_bar", "price_gt_vwap", 0.10),
        ("015", "YM", "NYP", "velez_20_200_elephant_bar", "rsi_lt_80", 0.10),
        ("016", "NQ", "ASN", "velez_20_200_elephant_bar", "macd_hist_gt_0", 0.05),
        ("017", "ES", "NYA", "velez_20_200_elephant_bar", "ema20_gt_sma200", 0.10),
        ("018", "YM", "LON", "velez_20_200_elephant_bar", "volume_gt_sma20", 0.20),
        ("019", "NQ", "NYP", "velez_20_200_elephant_bar", "adx_gt_35", 0.10),
        ("020", "ES", "ASN", "rosato_sd_absorption", "rsi_gt_70", 0.05),
        ("021", "YM", "LON", "rosato_sd_absorption", "price_gt_bb_upper", 0.15),
        ("022", "NQ", "NYA", "rosato_sd_absorption", "volume_gt_sma100", 0.10),
        ("023", "ES", "NYP", "rosato_sd_absorption", "macd_hist_lt_0", 0.10),
        ("024", "YM", "NYA", "rosato_sd_absorption", "price_lt_vwap", 0.20),
        ("025", "NQ", "LON", "rosato_sd_absorption", "adx_lt_20", 0.15),
        ("026", "ES", "ASN", "rosato_sd_absorption", "ema9_lt_ema20", 0.05),
        ("027", "YM", "NYP", "carter_ttm_squeeze", "adx_gt_25", 0.10),
        ("028", "NQ", "NYA", "carter_ttm_squeeze", "price_gt_sma200", 0.10),
        ("029", "ES", "LON", "carter_ttm_squeeze", "rsi_gt_50", 0.15),
        ("030", "YM", "ASN", "carter_ttm_squeeze", "ema20_gt_sma50", 0.05),
        ("031", "NQ", "NYP", "carter_ttm_squeeze", "volume_gt_sma20", 0.10),
        ("032", "ES", "NYA", "carter_ttm_squeeze", "macd_signal_cross_up", 0.10),
        ("033", "YM", "LON", "carter_ttm_squeeze", "price_gt_vwap", 0.20),
        ("034", "NQ", "ASN", "carter_ttm_squeeze", "adx_gt_30", 0.05),
        ("035", "ES", "NYA", "raschke_holy_grail", "adx_gt_30", 0.10),
        ("036", "YM", "NYP", "raschke_holy_grail", "price_gt_vwap", 0.10),
        ("037", "NQ", "LON", "raschke_holy_grail", "ema20_gt_sma200", 0.15),
        ("038", "ES", "ASN", "raschke_holy_grail", "rsi_lt_70", 0.05),
        ("039", "YM", "LON", "raschke_holy_grail", "macd_hist_gt_0", 0.20),
        ("040", "NQ", "NYP", "raschke_holy_grail", "volume_gt_sma50", 0.10),
        ("041", "ES", "NYA", "raschke_holy_grail", "adx_gt_40", 0.10),
        ("042", "YM", "ASN", "raschke_holy_grail", "price_gt_kc_upper", 0.05),
        ("043", "NQ", "NYA", "wade_pats_second_entry", "price_gt_ema21", 0.10),
        ("044", "ES", "NYP", "wade_pats_second_entry", "adx_gt_20", 0.10),
        ("045", "YM", "LON", "wade_pats_second_entry", "rsi_gt_40", 0.15),
        ("046", "NQ", "ASN", "wade_pats_second_entry", "macd_hist_gt_0", 0.05),
        ("047", "ES", "NYA", "wade_pats_second_entry", "volume_gt_sma20", 0.10),
        ("048", "YM", "NYP", "wade_pats_second_entry", "price_gt_vwap", 0.10),
        ("049", "NQ", "LON", "wade_pats_second_entry", "ema21_gt_sma200", 0.15),
        ("050", "ES", "ASN", "wade_pats_second_entry", "adx_gt_30", 0.05),
        ("051", "NQ", "LON", "ict_silver_bullet", "price_lt_vwap", 0.15),
        ("052", "ES", "NYP", "ict_silver_bullet", "adx_lt_30", 0.10),
        ("053", "YM", "ASN", "ict_silver_bullet", "ema9_lt_ema20", 0.05),
        ("054", "NQ", "NYA", "ict_silver_bullet", "macd_hist_lt_0", 0.10),
        ("055", "ES", "LON", "ict_silver_bullet", "price_lt_sma200", 0.15),
        ("056", "YM", "NYP", "ict_silver_bullet", "rsi_gt_30", 0.10),
        ("057", "NQ", "ASN", "ict_silver_bullet", "volume_gt_sma100", 0.05),
        ("058", "ES", "NYA", "casper_inverted_fvg", "adx_gt_40", 0.10),
        ("059", "YM", "NYP", "casper_inverted_fvg", "price_gt_vwap", 0.10),
        ("060", "NQ", "LON", "casper_inverted_fvg", "ema20_gt_sma50", 0.15),
        ("061", "ES", "ASN", "casper_inverted_fvg", "rsi_lt_60", 0.05),
        ("062", "YM", "LON", "casper_inverted_fvg", "macd_hist_gt_0", 0.20),
        ("063", "NQ", "NYP", "casper_inverted_fvg", "volume_gt_sma20", 0.10),
        ("064", "ES", "NYA", "casper_inverted_fvg", "price_gt_bb_upper", 0.10),
        ("065", "YM", "ASN", "velez_20_200_elephant_bar", "price_lt_vwap", 0.05),
        ("066", "NQ", "NYA", "velez_20_200_elephant_bar", "adx_gt_40", 0.10),
        ("067", "ES", "NYP", "velez_20_200_elephant_bar", "ema9_lt_ema20", 0.10),
        ("068", "YM", "LON", "velez_20_200_elephant_bar", "rsi_gt_30", 0.15),
        ("069", "NQ", "ASN", "velez_20_200_elephant_bar", "macd_hist_lt_0", 0.05),
        ("070", "ES", "LON", "velez_20_200_elephant_bar", "volume_gt_sma50", 0.15),
        ("071", "YM", "NYA", "velez_20_200_elephant_bar", "price_lt_sma200", 0.20),
        ("072", "NQ", "NYP", "velez_20_200_elephant_bar", "adx_gt_25", 0.10),
        ("073", "ES", "ASN", "rosato_sd_absorption", "price_gt_vwap", 0.05),
        ("074", "YM", "LON", "rosato_sd_absorption", "adx_gt_30", 0.15),
        ("075", "NQ", "NYA", "rosato_sd_absorption", "ema20_gt_sma200", 0.10),
        ("076", "ES", "NYP", "rosato_sd_absorption", "rsi_lt_80", 0.10),
        ("077", "YM", "NYA", "rosato_sd_absorption", "macd_hist_gt_0", 0.20),
        ("078", "NQ", "LON", "rosato_sd_absorption", "volume_gt_sma20", 0.15),
        ("079", "ES", "ASN", "rosato_sd_absorption", "price_gt_kc_lower", 0.05),
        ("080", "YM", "NYP", "rosato_sd_absorption", "adx_gt_40", 0.10),
        ("081", "NQ", "NYA", "carter_ttm_squeeze", "price_lt_vwap", 0.10),
        ("082", "ES", "LON", "carter_ttm_squeeze", "ema9_lt_ema20", 0.15),
        ("083", "YM", "ASN", "carter_ttm_squeeze", "rsi_gt_30", 0.05),
        ("084", "NQ", "NYP", "carter_ttm_squeeze", "macd_hist_lt_0", 0.10),
        ("085", "ES", "NYA", "carter_ttm_squeeze", "volume_gt_sma50", 0.10),
        ("086", "YM", "LON", "carter_ttm_squeeze", "price_lt_sma200", 0.20),
        ("087", "NQ", "ASN", "carter_ttm_squeeze", "adx_gt_40", 0.05),
        ("088", "ES", "NYA", "raschke_holy_grail", "price_lt_vwap", 0.10),
        ("089", "YM", "NYP", "raschke_holy_grail", "ema9_lt_ema20", 0.10),
        ("090", "NQ", "LON", "raschke_holy_grail", "rsi_gt_30", 0.15),
        ("091", "ES", "ASN", "raschke_holy_grail", "macd_hist_lt_0", 0.05),
        ("092", "YM", "LON", "raschke_holy_grail", "volume_gt_sma20", 0.20),
        ("093", "NQ", "NYP", "raschke_holy_grail", "price_lt_sma200", 0.10),
        ("094", "ES", "NYA", "raschke_holy_grail", "adx_lt_40", 0.10),
        ("095", "YM", "ASN", "wade_pats_second_entry", "price_lt_ema21", 0.05),
        ("096", "NQ", "NYA", "wade_pats_second_entry", "adx_gt_40", 0.10),
        ("097", "ES", "NYP", "wade_pats_second_entry", "rsi_lt_60", 0.10),
        ("098", "YM", "LON", "wade_pats_second_entry", "macd_hist_lt_0", 0.15),
        ("099", "NQ", "ASN", "wade_pats_second_entry", "volume_gt_sma50", 0.05),
        ("100", "ES", "NYA", "wade_pats_second_entry", "price_lt_vwap", 0.10),
    ]

    for sid, instrument, session, blueprint, filter_expr, tp_atr in _raw:
        cfg = default_params()
        cfg["blueprint"] = blueprint
        cfg["instrument"] = instrument
        cfg["session"] = session
        cfg["filter"] = filter_expr
        cfg["tp_atr"] = tp_atr
        cfg["sl_atr"] = 5.0
        sess = SESSIONS.get(session, {})
        cfg["session_start"] = sess.get("start", "09:30")
        cfg["session_end"] = sess.get("end", "16:00")
        rows.append(
            {
                "id": sid,
                "blueprint": blueprint,
                "name": f"P1_{sid}_{blueprint}_{instrument}_{session}_{filter_expr}_tp{tp_atr}_sl5.0",
                "params": cfg,
            }
        )

    if len(rows) != 100:
        raise RuntimeError(f"Paper-1 matrix must contain exactly 100 rows, got {len(rows)}")

    return rows


_MATRIX: List[Dict[str, Any]] = _build_matrix()


# -----------------------------------------------------------------------------
# Public matrix accessors
# -----------------------------------------------------------------------------

def list_strategy_ids() -> List[str]:
    """Return sorted Paper-1 strategy IDs ("001" through "100")."""
    return [row["id"] for row in _MATRIX]


def get_strategy_config(id: str) -> Dict[str, Any]:
    """Return the parameter dict for a given Paper-1 matrix ID.

    Parameters
    ----------
    id : str
        Three-digit ID from "001" to "100".

    Returns
    -------
    dict
        Strategy parameters ready for ``generate_signals``.
    """
    for row in _MATRIX:
        if row["id"] == id:
            return row["params"].copy()
    raise ValueError(f"Unknown Paper-1 strategy ID: {id!r}")


def get_matrix_df() -> pd.DataFrame:
    """Return the full 100-row Paper-1 matrix as a DataFrame."""
    return pd.DataFrame(
        [
            {
                "id": row["id"],
                "blueprint": row["blueprint"],
                "name": row["name"],
                **{f"param_{k}": v for k, v in row["params"].items()},
            }
            for row in _MATRIX
        ]
    )


# -----------------------------------------------------------------------------
# Indicator helpers (closed-bar only)
# -----------------------------------------------------------------------------

def _sma(series: pd.Series, length: int) -> pd.Series:
    return series.rolling(length, min_periods=length).mean().shift(1)


def _ema(series: pd.Series, length: int) -> pd.Series:
    return series.ewm(span=length, adjust=False, min_periods=length).mean().shift(1)


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


def _rsi(series: pd.Series, length: int = 14) -> pd.Series:
    delta = series.diff()
    gain = delta.where(delta > 0, 0.0)
    loss = (-delta).where(delta < 0, 0.0)
    avg_gain = gain.rolling(length, min_periods=length).mean().shift(1)
    avg_loss = loss.rolling(length, min_periods=length).mean().shift(1)
    rs = avg_gain / avg_loss.replace(0, np.nan)
    return 100.0 - (100.0 / (1.0 + rs))


def _macd_hist(series: pd.Series, fast: int = 12, slow: int = 26, signal: int = 9) -> pd.Series:
    ema_fast = series.ewm(span=fast, adjust=False, min_periods=fast).mean().shift(1)
    ema_slow = series.ewm(span=slow, adjust=False, min_periods=slow).mean().shift(1)
    macd = ema_fast - ema_slow
    signal_line = macd.ewm(span=signal, adjust=False, min_periods=signal).mean().shift(1)
    return macd - signal_line


def _vwap(df: pd.DataFrame) -> pd.Series:
    """Daily anchored VWAP (closed bar only)."""
    local = df.index.tz_convert("America/New_York")
    date = local.date
    typical = (df["high"] + df["low"] + df["close"]) / 3.0
    vol = df["volume"].astype(float)
    cum_typ_vol = (typical * vol).groupby(date).cumsum().shift(1)
    cum_vol = vol.groupby(date).cumsum().shift(1)
    return cum_typ_vol / cum_vol.replace(0, np.nan)


def _volume_sma(df: pd.DataFrame, length: int) -> pd.Series:
    return df["volume"].rolling(length, min_periods=length).mean().shift(1)


def _bollinger(df: pd.DataFrame, length: int, std_dev: float) -> Tuple[pd.Series, pd.Series, pd.Series]:
    mid = _sma(df["close"], length)
    std = df["close"].rolling(length, min_periods=length).std().shift(1)
    upper = mid + std_dev * std
    lower = mid - std_dev * std
    return upper, mid, lower


def _keltner(df: pd.DataFrame, length: int, mult: float) -> Tuple[pd.Series, pd.Series, pd.Series]:
    mid = _ema(df["close"], length)
    atr = _atr(df, length)
    upper = mid + mult * atr
    lower = mid - mult * atr
    return upper, mid, lower


def _adx(df: pd.DataFrame, length: int) -> pd.Series:
    """Welles Wilder ADX using only closed-bar data."""
    high = df["high"]
    low = df["low"]

    plus_dm = (high - high.shift(1)).clip(lower=0)
    minus_dm = (low.shift(1) - low).clip(lower=0)
    plus_dm = plus_dm.where(plus_dm > minus_dm, 0.0)
    minus_dm = minus_dm.where(minus_dm > plus_dm, 0.0)

    tr = _true_range(df)
    atr = tr.rolling(length, min_periods=length).mean().shift(1)

    plus_di = 100.0 * plus_dm.rolling(length, min_periods=length).mean().shift(1) / atr.replace(0, np.nan)
    minus_di = 100.0 * minus_dm.rolling(length, min_periods=length).mean().shift(1) / atr.replace(0, np.nan)

    dx = (
        (plus_di - minus_di).abs()
        / (plus_di + minus_dm).replace(0, np.nan)
        * 100.0
    )
    adx = dx.rolling(length, min_periods=length).mean().shift(1)
    return adx.fillna(0.0)


def _fvg_detect(df: pd.DataFrame) -> Tuple[pd.Series, pd.Series]:
    """Return bullish and bearish Fair Value Gap arrays.

    A bullish FVG exists at index i when low[i] > high[i-2].
    A bearish FVG exists at index i when high[i] < low[i-2].
    Returns the top/bottom of the most recent unfilled gap for each bar.
    """
    low = df["low"].values
    high = df["high"].values
    n = len(df)

    bull_top = np.full(n, np.nan)
    bull_bottom = np.full(n, np.nan)
    bear_top = np.full(n, np.nan)
    bear_bottom = np.full(n, np.nan)

    last_bull_top = np.nan
    last_bull_bottom = np.nan
    last_bear_top = np.nan
    last_bear_bottom = np.nan

    for i in range(2, n):
        # Invalidate existing gaps that have been filled.
        if not np.isnan(last_bull_bottom) and low[i] <= last_bull_bottom:
            last_bull_top = np.nan
            last_bull_bottom = np.nan
        if not np.isnan(last_bear_top) and high[i] >= last_bear_top:
            last_bear_top = np.nan
            last_bear_bottom = np.nan

        # New bullish FVG: current low above high two bars ago.
        if low[i] > high[i - 2]:
            last_bull_bottom = high[i - 2]
            last_bull_top = low[i]

        # New bearish FVG: current high below low two bars ago.
        if high[i] < low[i - 2]:
            last_bear_top = low[i - 2]
            last_bear_bottom = high[i]

        bull_top[i] = last_bull_top
        bull_bottom[i] = last_bull_bottom
        bear_top[i] = last_bear_top
        bear_bottom[i] = last_bear_bottom

    idx = df.index
    return (
        pd.DataFrame(
            {"top": bull_top, "bottom": bull_bottom}, index=idx
        ),
        pd.DataFrame(
            {"top": bear_top, "bottom": bear_bottom}, index=idx
        ),
    )


def _swing_highs_lows(df: pd.DataFrame, lookback: int) -> Tuple[pd.Series, pd.Series]:
    """Return boolean series marking swing highs and swing lows."""
    high = df["high"]
    low = df["low"]
    roll_high = high.rolling(2 * lookback + 1, center=True, min_periods=lookback).max()
    roll_low = low.rolling(2 * lookback + 1, center=True, min_periods=lookback).min()
    swing_high = (high == roll_high).shift(1).fillna(False)
    swing_low = (low == roll_low).shift(1).fillna(False)
    return swing_high, swing_low


def _session_date(index_utc: pd.DatetimeIndex, tz: str) -> pd.Series:
    return pd.Series(index_utc.tz_convert(tz).date, index=index_utc)


def _time_in_window(index_utc: pd.DatetimeIndex, tz: str, start: str, end: str) -> pd.Series:
    local = index_utc.tz_convert(tz)
    start_t = pd.Timestamp(start).time()
    end_t = pd.Timestamp(end).time()
    times = local.time
    if start_t < end_t:
        return pd.Series((times >= start_t) & (times <= end_t), index=index_utc)
    return pd.Series((times >= start_t) | (times <= end_t), index=index_utc)


# -----------------------------------------------------------------------------
# Filter evaluation
# -----------------------------------------------------------------------------

def _build_filter_mask(df: pd.DataFrame, filter_expr: Optional[str]) -> pd.Series:
    """Evaluate a Paper-1 filter expression using only closed-bar data."""
    if filter_expr is None or filter_expr == "none":
        return pd.Series(True, index=df.index)

    close = df["close"]
    if filter_expr == "adx_gt_20":
        return _adx(df, 14) > 20
    if filter_expr == "adx_gt_25":
        return _adx(df, 14) > 25
    if filter_expr == "adx_gt_30":
        return _adx(df, 14) > 30
    if filter_expr == "adx_gt_35":
        return _adx(df, 14) > 35
    if filter_expr == "adx_gt_40":
        return _adx(df, 14) > 40
    if filter_expr == "rsi_lt_70":
        return _rsi(close, 14) < 70
    if filter_expr == "rsi_gt_30":
        return _rsi(close, 14) > 30
    if filter_expr == "rsi_lt_80":
        return _rsi(close, 14) < 80
    if filter_expr == "rsi_gt_40":
        return _rsi(close, 14) > 40
    if filter_expr == "rsi_lt_60":
        return _rsi(close, 14) < 60
    if filter_expr == "price_gt_vwap":
        return close > _vwap(df)
    if filter_expr == "price_lt_vwap":
        return close < _vwap(df)
    if filter_expr == "ema20_gt_ema50":
        return _ema(close, 20) > _sma(close, 50)
    if filter_expr == "ema20_gt_sma50":
        return _ema(close, 20) > _sma(close, 50)
    if filter_expr == "ema9_lt_ema20":
        return _ema(close, 9) < _ema(close, 20)
    if filter_expr == "ema9_lt_ema21":
        return _ema(close, 9) < _ema(close, 21)
    if filter_expr == "volume_gt_sma20":
        return df["volume"] > _volume_sma(df, 20)
    if filter_expr == "volume_gt_sma50":
        return df["volume"] > _volume_sma(df, 50)
    if filter_expr == "volume_gt_sma100":
        return df["volume"] > _volume_sma(df, 100)
    if filter_expr == "macd_hist_gt_0":
        return _macd_hist(close, 12, 26, 9) > 0
    if filter_expr == "macd_hist_lt_0":
        return _macd_hist(close, 12, 26, 9) < 0
    if filter_expr == "price_gt_sma200":
        return close > _sma(close, 200)
    if filter_expr == "price_lt_sma200":
        return close < _sma(close, 200)
    if filter_expr == "price_gt_bb_upper":
        upper, _, _ = _bollinger(df, 20, 2.0)
        return close > upper
    if filter_expr == "price_gt_kc_lower":
        _, _, lower = _keltner(df, 20, 1.5)
        return close > lower
    if filter_expr == "price_lt_kc_upper":
        upper, _, _ = _keltner(df, 20, 1.5)
        return close < upper
    if filter_expr == "macd_signal_cross_up":
        macd_hist = _macd_hist(close, 12, 26, 9)
        return (macd_hist > 0) & (macd_hist.shift(1) <= 0)

    # Additional filters required by the exact Paper-1 matrix.
    if filter_expr == "adx_lt_20":
        return _adx(df, 14) < 20
    if filter_expr == "adx_lt_30":
        return _adx(df, 14) < 30
    if filter_expr == "adx_lt_40":
        return _adx(df, 14) < 40
    if filter_expr == "rsi_gt_50":
        return _rsi(close, 14) > 50
    if filter_expr == "rsi_gt_70":
        return _rsi(close, 14) > 70
    if filter_expr == "ema20_gt_sma200":
        return _ema(close, 20) > _sma(close, 200)
    if filter_expr == "ema21_gt_sma200":
        return _ema(close, 21) > _sma(close, 200)
    if filter_expr == "price_gt_ema21":
        return close > _ema(close, 21)
    if filter_expr == "price_lt_ema21":
        return close < _ema(close, 21)
    if filter_expr == "price_gt_kc_upper":
        upper, _, _ = _keltner(df, 20, 1.5)
        return close > upper

    raise ValueError(f"Unknown Paper-1 filter expression: {filter_expr!r}")


# -----------------------------------------------------------------------------
# Trade simulation
# -----------------------------------------------------------------------------

def _simulate_exit(
    df: pd.DataFrame,
    entry_time: pd.Timestamp,
    direction: int,
    entry_price: float,
    stop_loss: float,
    take_profit: float,
    session_end_time: Optional[Any] = None,
    stop_first: bool = True,
) -> Tuple[pd.Timestamp, float, str, float]:
    """Walk forward from entry_time and return the first exit."""
    future = df.loc[df.index > entry_time]
    if future.empty:
        last = df.iloc[-1]
        pnl = direction * (last["close"] - entry_price)
        return df.index[-1], last["close"], "end_of_data", pnl

    for ts, bar in future.iterrows():
        if session_end_time is not None:
            local_time = ts.tz_convert("America/New_York").time()
            if local_time >= session_end_time:
                pnl = direction * (bar["close"] - entry_price)
                return ts, bar["close"], "session_end", pnl

        if direction == 1:
            sl_hit = bar["low"] <= stop_loss
            tp_hit = bar["high"] >= take_profit
        else:
            sl_hit = bar["high"] >= stop_loss
            tp_hit = bar["low"] <= take_profit

        if sl_hit and tp_hit:
            exit_price = stop_loss if stop_first else take_profit
            reason = "sl" if stop_first else "tp"
        elif sl_hit:
            exit_price = stop_loss
            reason = "sl"
        elif tp_hit:
            exit_price = take_profit
            reason = "tp"
        else:
            continue

        pnl = direction * (exit_price - entry_price)
        return ts, exit_price, reason, pnl

    last = future.iloc[-1]
    pnl = direction * (last["close"] - entry_price)
    return last.name, last["close"], "end_of_data", pnl


def _add_local_meta(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Add local-time, date, and session columns without lookahead."""
    tz = cfg["tz"]
    local = df.index.tz_convert(tz)
    df = df.copy()
    df["_local"] = local
    df["_date"] = local.date
    df["_time"] = local.time
    df["_in_session"] = data.get_session_mask(
        df, start_time=cfg["session_start"], end_time=cfg["session_end"], tz=tz
    )
    return df


def _entry_exit(
    df: pd.DataFrame,
    entry_time: pd.Timestamp,
    direction: int,
    entry_price: float,
    atr_value: float,
    cfg: Dict[str, Any],
) -> Optional[Dict[str, Any]]:
    """Build a trade dict with TP/SL from ATR multipliers and simulate exit."""
    tp_atr = float(cfg.get("tp_atr", 2.0))
    sl_atr = float(cfg.get("sl_atr", 5.0))
    tick_size = float(cfg.get("tick_size", 0.25))

    if direction == 1:
        stop_loss = entry_price - sl_atr * atr_value - tick_size
        take_profit = entry_price + tp_atr * atr_value + tick_size
    else:
        stop_loss = entry_price + sl_atr * atr_value + tick_size
        take_profit = entry_price - tp_atr * atr_value - tick_size

    if direction == 1 and not (take_profit > entry_price > stop_loss):
        return None
    if direction == -1 and not (take_profit < entry_price < stop_loss):
        return None

    et, ep, er, pnl = _simulate_exit(
        df,
        entry_time,
        direction,
        entry_price,
        stop_loss,
        take_profit,
        session_end_time=pd.Timestamp(cfg["session_end"]).time() if cfg.get("session_only") else None,
        stop_first=cfg.get("stop_first", True),
    )
    return {
        "entry_time": entry_time,
        "direction": direction,
        "entry_price": entry_price,
        "stop_loss": stop_loss,
        "take_profit": take_profit,
        "exit_time": et,
        "exit_price": ep,
        "pnl": pnl,
        "exit_reason": er,
    }


# -----------------------------------------------------------------------------
# Blueprint implementations
# -----------------------------------------------------------------------------

def _ict_silver_bullet(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """ICT Silver Bullet: sweep of a prior level in a killzone, then 50%
    retracement / FVG confluence entry.
    """
    sweep_tf = cfg.get("sweep_timeframe", "15m")
    entry_tf = cfg.get("entry_timeframe", "1m")
    fvg_required = cfg.get("fvg_required", True)
    retracement_pct = float(cfg.get("retracement_pct", 0.5))

    # Higher-timeframe bars define the level to sweep.
    htf = data.resample_timeframe(df, sweep_tf)
    htf["prev_high"] = htf["high"].shift(1)
    htf["prev_low"] = htf["low"].shift(1)

    # Entry-timeframe bars.
    entry_df = df if entry_tf == "1m" else data.resample_timeframe(df, entry_tf)
    entry_df = _add_local_meta(entry_df, cfg)

    atr = _atr(entry_df, int(cfg.get("atr_length", 14)))
    bull_fvg, bear_fvg = _fvg_detect(entry_df)
    filter_mask = _build_filter_mask(entry_df, cfg.get("filter"))

    trades: List[Dict[str, Any]] = []
    last_date: Any = None

    for htf_time, htf_row in htf.iterrows():
        prev_high = htf_row["prev_high"]
        prev_low = htf_row["prev_low"]
        if pd.isna(prev_high) or pd.isna(prev_low):
            continue

        # Candidate entry bars that fall inside this HTF interval.
        slice_entry = entry_df.loc[
            (entry_df.index > htf_time - pd.Timedelta(sweep_tf))
            & (entry_df.index <= htf_time)
        ]
        if cfg.get("session_only"):
            slice_entry = slice_entry[slice_entry["_in_session"]]
        if slice_entry.empty:
            continue

        for date, day_slice in slice_entry.groupby("_date"):
            if cfg.get("one_trade_per_day") and last_date == date:
                continue

            day_atr = atr.loc[day_slice.index]
            atr_value = day_atr.dropna().iloc[0] if not day_atr.dropna().empty else 0.0
            if atr_value <= 0:
                continue

            filt = filter_mask.loc[day_slice.index]

            # Long setup: sweep prev_low then retracement into the 50% zone.
            swept_low = (day_slice["low"] <= prev_low).cumsum() > 0
            mid = (prev_high + prev_low) * 0.5
            retr_zone_top = prev_low + retracement_pct * (mid - prev_low)
            in_zone = (day_slice["low"] <= retr_zone_top) & (day_slice["high"] >= prev_low)

            long_fvg_ok = True
            if fvg_required:
                long_fvg_ok = bull_fvg.loc[day_slice.index].notna().any()

            long_cond = swept_low & in_zone & long_fvg_ok & filt
            if long_cond.any():
                bar = day_slice[long_cond].iloc[0]
                entry_price = float(bar["close"])
                trade = _entry_exit(df, bar.name, 1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date
                    break

            # Short setup: sweep prev_high then retracement.
            swept_high = (day_slice["high"] >= prev_high).cumsum() > 0
            retr_zone_bottom = prev_high - retracement_pct * (prev_high - mid)
            in_zone_short = (day_slice["high"] >= retr_zone_bottom) & (day_slice["low"] <= prev_high)

            short_fvg_ok = True
            if fvg_required:
                short_fvg_ok = bear_fvg.loc[day_slice.index].notna().any()

            short_cond = swept_high & in_zone_short & short_fvg_ok & filt
            if short_cond.any():
                bar = day_slice[short_cond].iloc[0]
                entry_price = float(bar["close"])
                trade = _entry_exit(df, bar.name, -1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date
                    break

    return _signals_from_trades(trades)


def _casper_inverted_fvg(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Casper SMC Inverted FVG: price fills an FVG, then closes back inside
    the gap in the direction of the gap.
    """
    fvg_tf = cfg.get("fvg_timeframe", "5m")
    entry_model = cfg.get("entry_model", "close_back_inside")

    tf_df = data.resample_timeframe(df, fvg_tf)
    tf_df = _add_local_meta(tf_df, cfg)
    bull_fvg, bear_fvg = _fvg_detect(tf_df)
    atr = _atr(tf_df, int(cfg.get("atr_length", 14)))
    filter_mask = _build_filter_mask(tf_df, cfg.get("filter"))

    trades: List[Dict[str, Any]] = []
    last_date: Any = None

    for i in range(2, len(tf_df)):
        ts = tf_df.index[i]
        if cfg.get("session_only") and not tf_df["_in_session"].iloc[i]:
            continue

        date = tf_df["_date"].iloc[i]
        if cfg.get("one_trade_per_day") and last_date == date:
            continue

        if not filter_mask.iloc[i]:
            continue

        row = tf_df.iloc[i]
        close = float(row["close"])
        high = float(row["high"])
        low = float(row["low"])
        atr_value = atr.iloc[i]
        if pd.isna(atr_value) or atr_value <= 0:
            continue

        # Inverted FVGs are evaluated on the gap that existed at the previous
        # bar; using the current bar's prices for fill/confirmation avoids
        # having _fvg_detect invalidate the gap on the same bar it is filled.
        prev = i - 1

        # Bearish inverted FVG: bullish gap gets filled and retested -> short.
        gap_bottom = bull_fvg["bottom"].iloc[prev]
        gap_top = bull_fvg["top"].iloc[prev]
        if not pd.isna(gap_bottom):
            filled = low <= gap_bottom
            if entry_model == "close_back_inside":
                confirmed = close < gap_bottom
            else:
                confirmed = high > gap_top
            if filled and confirmed:
                entry_price = close
                trade = _entry_exit(df, ts, -1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date
                    continue

        # Bullish inverted FVG: bearish gap gets filled and retested -> long.
        gap_top = bear_fvg["top"].iloc[prev]
        gap_bottom = bear_fvg["bottom"].iloc[prev]
        if not pd.isna(gap_top):
            filled = high >= gap_top
            if entry_model == "close_back_inside":
                confirmed = close > gap_top
            else:
                confirmed = low < gap_bottom
            if filled and confirmed:
                entry_price = close
                trade = _entry_exit(df, ts, 1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date

    return _signals_from_trades(trades)


def _velez_20_200_elephant_bar(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Velez 20/200 Elephant Bar: large-range bar closing near its extreme,
    aligned with the 20 EMA vs 200 SMA trend.
    """
    ema_fast = int(cfg.get("ema_fast", 20))
    ema_slow = int(cfg.get("ema_slow", 200))
    min_range_atr = float(cfg.get("min_range_atr", 1.0))
    close_position = cfg.get("close_position", "extreme")

    df1 = _add_local_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["ema_fast"] = _ema(df1["close"], ema_fast)
    df1["sma_slow"] = _sma(df1["close"], ema_slow)
    filter_mask = _build_filter_mask(df1, cfg.get("filter"))

    trades: List[Dict[str, Any]] = []
    last_date: Any = None

    for i in range(1, len(df1)):
        row = df1.iloc[i]
        if cfg.get("session_only") and not row["_in_session"]:
            continue

        date = row["_date"]
        if cfg.get("one_trade_per_day") and last_date == date:
            continue

        atr_value = row["atr"]
        if pd.isna(atr_value) or atr_value <= 0:
            continue

        if not filter_mask.iloc[i]:
            continue

        bar_range = row["high"] - row["low"]
        if bar_range < atr_value * min_range_atr:
            continue

        close = row["close"]
        high = row["high"]
        low = row["low"]
        ema20 = row["ema_fast"]
        sma200 = row["sma_slow"]

        # Long: close near high, above fast EMA and slow SMA.
        if close > ema20 and close > sma200:
            near_high = (high - close) <= 0.2 * bar_range if close_position == "extreme" else True
            if near_high:
                entry_price = float(close)
                trade = _entry_exit(df, df1.index[i], 1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date
                    continue

        # Short: close near low, below fast EMA and slow SMA.
        if close < ema20 and close < sma200:
            near_low = (close - low) <= 0.2 * bar_range if close_position == "extreme" else True
            if near_low:
                entry_price = float(close)
                trade = _entry_exit(df, df1.index[i], -1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date

    return _signals_from_trades(trades)


def _rosato_sd_absorption(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Rosato Supply/Demand Absorption: high-volume test of a prior zone with
    diminishing follow-through.
    """
    zone_tf = cfg.get("zone_timeframe", "15m")
    vol_pct = float(cfg.get("volume_percentile", 80))
    abs_bars = int(cfg.get("absorption_bars", 3))

    zf = data.resample_timeframe(df, zone_tf)
    zf = _add_local_meta(zf, cfg)
    zf["zone_high"] = zf["high"].shift(1)
    zf["zone_low"] = zf["low"].shift(1)

    vol_threshold = zf["volume"].rolling(50, min_periods=20).quantile(vol_pct / 100.0).shift(1)
    atr = _atr(zf, int(cfg.get("atr_length", 14)))
    ema20 = _ema(zf["close"], 20)
    filter_mask = _build_filter_mask(zf, cfg.get("filter"))

    # Demand / supply detection on the higher timeframe.
    demand = (zf["low"] <= zf["zone_low"]) & (zf["close"] > zf["open"]) & (zf["volume"] > vol_threshold)
    supply = (zf["high"] >= zf["zone_high"]) & (zf["close"] < zf["open"]) & (zf["volume"] > vol_threshold)

    trades: List[Dict[str, Any]] = []
    last_date: Any = None

    for i in range(abs_bars + 1, len(zf)):
        ts = zf.index[i]
        if cfg.get("session_only") and not zf["_in_session"].iloc[i]:
            continue

        date = zf["_date"].iloc[i]
        if cfg.get("one_trade_per_day") and last_date == date:
            continue

        if not filter_mask.iloc[i]:
            continue

        atr_value = atr.iloc[i]
        if pd.isna(atr_value) or atr_value <= 0:
            continue

        # Demand absorption: price comes back to a demand zone, volume dries up,
        # and a bullish close confirms.
        if demand.iloc[i - 1] or demand.iloc[i - 2]:
            recent = zf.iloc[i - abs_bars : i]
            in_zone = (recent["low"] <= zf["zone_low"].iloc[i]).any()
            drying = recent["volume"].iloc[-1] < recent["volume"].mean()
            confirm = zf["close"].iloc[i] > zf["open"].iloc[i]
            trend_ok = zf["close"].iloc[i] > ema20.iloc[i]
            if in_zone and drying and confirm and trend_ok:
                entry_price = float(zf["close"].iloc[i])
                trade = _entry_exit(df, ts, 1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date
                    continue

        # Supply absorption.
        if supply.iloc[i - 1] or supply.iloc[i - 2]:
            recent = zf.iloc[i - abs_bars : i]
            in_zone = (recent["high"] >= zf["zone_high"].iloc[i]).any()
            drying = recent["volume"].iloc[-1] < recent["volume"].mean()
            confirm = zf["close"].iloc[i] < zf["open"].iloc[i]
            trend_ok = zf["close"].iloc[i] < ema20.iloc[i]
            if in_zone and drying and confirm and trend_ok:
                entry_price = float(zf["close"].iloc[i])
                trade = _entry_exit(df, ts, -1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date

    return _signals_from_trades(trades)


def _carter_ttm_squeeze(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Carter TTM Squeeze: momentum breakout after Bollinger Bands move inside
    Keltner Channels.
    """
    bb_len = int(cfg.get("bb_length", 20))
    kc_mult = float(cfg.get("kc_mult", 1.5))
    mom_len = int(cfg.get("momentum_length", 12))
    direction = cfg.get("direction", "both")

    df1 = _add_local_meta(df, cfg)
    bb_upper, _, bb_lower = _bollinger(df1, bb_len, 2.0)
    kc_upper, _, kc_lower = _keltner(df1, bb_len, kc_mult)

    # Squeeze is on when BB are inside KC.
    squeeze_on = (bb_upper <= kc_upper) & (bb_lower >= kc_lower)
    # Squeeze fires when squeeze ends.
    squeeze_fired = squeeze_on.shift(1) & (~squeeze_on)

    # Momentum via simple rate-of-change.
    momentum = df1["close"].diff(mom_len).shift(1)
    atr = _atr(df1, int(cfg.get("atr_length", 14)))
    filter_mask = _build_filter_mask(df1, cfg.get("filter"))

    trades: List[Dict[str, Any]] = []
    last_date: Any = None

    for i in range(2, len(df1)):
        row = df1.iloc[i]
        if cfg.get("session_only") and not row["_in_session"]:
            continue

        date = row["_date"]
        if cfg.get("one_trade_per_day") and last_date == date:
            continue

        if not squeeze_fired.iloc[i]:
            continue

        if not filter_mask.iloc[i]:
            continue

        atr_value = atr.iloc[i]
        if pd.isna(atr_value) or atr_value <= 0:
            continue

        close = row["close"]
        mom = momentum.iloc[i]
        if pd.isna(mom):
            continue

        if direction in ("both", "long") and mom > 0:
            entry_price = float(close)
            trade = _entry_exit(df, df1.index[i], 1, entry_price, atr_value, cfg)
            if trade is not None:
                trades.append(trade)
                last_date = date
                continue

        if direction in ("both", "short") and mom < 0:
            entry_price = float(close)
            trade = _entry_exit(df, df1.index[i], -1, entry_price, atr_value, cfg)
            if trade is not None:
                trades.append(trade)
                last_date = date

    return _signals_from_trades(trades)


def _raschke_holy_grail(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Raschke Holy Grail: pullback to a rising/falling 20 EMA in a trending
    market filtered by ADX.
    """
    ema_len = int(cfg.get("ema_length", 20))
    adx_len = int(cfg.get("adx_length", 14))
    adx_thr = float(cfg.get("adx_threshold", 25))
    pullback_depth = float(cfg.get("pullback_depth", 0.5))

    df1 = _add_local_meta(df, cfg)
    df1["ema"] = _ema(df1["close"], ema_len)
    df1["adx"] = _adx(df1, adx_len)
    df1["ema_slope"] = df1["ema"].diff(3)
    atr = _atr(df1, int(cfg.get("atr_length", 14)))
    filter_mask = _build_filter_mask(df1, cfg.get("filter"))

    trades: List[Dict[str, Any]] = []
    last_date: Any = None

    for i in range(2, len(df1)):
        row = df1.iloc[i]
        if cfg.get("session_only") and not row["_in_session"]:
            continue

        date = row["_date"]
        if cfg.get("one_trade_per_day") and last_date == date:
            continue

        ema = row["ema"]
        adx = row["adx"]
        slope = row["ema_slope"]
        close = row["close"]
        low = row["low"]
        high = row["high"]

        if pd.isna(ema) or pd.isna(adx) or pd.isna(slope):
            continue

        atr_value = atr.iloc[i]
        if pd.isna(atr_value) or atr_value <= 0:
            continue

        if not filter_mask.iloc[i]:
            continue

        # Long: rising EMA, ADX strong, price pulls back to EMA and closes back above.
        if slope > 0 and adx >= adx_thr:
            dist_to_ema = max(0.0, ema - low)
            total_range = high - low
            if total_range > 0 and dist_to_ema >= pullback_depth * total_range and close > ema:
                entry_price = float(high) + float(cfg.get("tick_size", 0.25))
                trade = _entry_exit(df, df1.index[i], 1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date
                    continue

        # Short: falling EMA, ADX strong, price pulls back to EMA and closes back below.
        if slope < 0 and adx >= adx_thr:
            dist_to_ema = max(0.0, high - ema)
            total_range = high - low
            if total_range > 0 and dist_to_ema >= pullback_depth * total_range and close < ema:
                entry_price = float(low) - float(cfg.get("tick_size", 0.25))
                trade = _entry_exit(df, df1.index[i], -1, entry_price, atr_value, cfg)
                if trade is not None:
                    trades.append(trade)
                    last_date = date

    return _signals_from_trades(trades)


def _wade_pats_second_entry(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Wade PATs Second Entry: after a failed first break of a swing high/low,
    enter on the second successful attempt.
    """
    swing_lb = int(cfg.get("swing_lookback", 10))
    trend_filter = cfg.get("trend_filter", False)

    df1 = _add_local_meta(df, cfg)
    swing_high, swing_low = _swing_highs_lows(df1, swing_lb)
    ema20 = _ema(df1["close"], 20)
    atr = _atr(df1, int(cfg.get("atr_length", 14)))
    filter_mask = _build_filter_mask(df1, cfg.get("filter"))

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    last_swing_high: Optional[float] = None
    last_swing_low: Optional[float] = None
    high_failures = 0
    low_failures = 0

    for i in range(1, len(df1)):
        row = df1.iloc[i]
        if cfg.get("session_only") and not row["_in_session"]:
            continue

        date = row["_date"]
        if cfg.get("one_trade_per_day") and last_date == date:
            continue

        if not filter_mask.iloc[i]:
            continue

        atr_value = atr.iloc[i]
        if pd.isna(atr_value) or atr_value <= 0:
            continue

        # _swing_highs_lows marks the bar AFTER the peak, so the level is the
        # high/low of the previous bar, not the current bar.
        if swing_high.iloc[i]:
            last_swing_high = float(df1.iloc[i - 1]["high"])
            high_failures = 0
        if swing_low.iloc[i]:
            last_swing_low = float(df1.iloc[i - 1]["low"])
            low_failures = 0

        close = row["close"]
        high = row["high"]
        low = row["low"]

        # Long second entry: price briefly broke above swing high and failed,
        # now closes back above it.
        if last_swing_high is not None:
            if high > last_swing_high and close < last_swing_high:
                high_failures += 1
            if high_failures >= 1:
                confirmed = close > last_swing_high
                trend_ok = (not trend_filter) or (close > ema20.iloc[i])
                if confirmed and trend_ok:
                    entry_price = float(high) + float(cfg.get("tick_size", 0.25))
                    trade = _entry_exit(df, df1.index[i], 1, entry_price, atr_value, cfg)
                    if trade is not None:
                        trades.append(trade)
                        last_date = date
                        high_failures = 0
                        continue

        # Short second entry.
        if last_swing_low is not None:
            if low < last_swing_low and close > last_swing_low:
                low_failures += 1
            if low_failures >= 1:
                confirmed = close < last_swing_low
                trend_ok = (not trend_filter) or (close < ema20.iloc[i])
                if confirmed and trend_ok:
                    entry_price = float(low) - float(cfg.get("tick_size", 0.25))
                    trade = _entry_exit(df, df1.index[i], -1, entry_price, atr_value, cfg)
                    if trade is not None:
                        trades.append(trade)
                        last_date = date
                        low_failures = 0

    return _signals_from_trades(trades)


# -----------------------------------------------------------------------------
# Public entry point
# -----------------------------------------------------------------------------

def generate_signals(df_1m: pd.DataFrame, params: Optional[Dict[str, Any]] = None) -> pd.DataFrame:
    """Generate Paper-1 strategy signals from 1-minute futures data.

    Parameters
    ----------
    df_1m : pd.DataFrame
        1-minute OHLCV data with a UTC DatetimeIndex.
    params : dict, optional
        Strategy parameters.  Must contain ``blueprint`` selecting one of the
        seven Paper-1 blueprints.  Use ``get_strategy_config(id)`` to obtain a
        ready-made parameter set from the 100-row matrix.

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

    # Ensure UTC index.
    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    df.index = df.index.tz_convert("UTC")

    # Apply session override if a known session key is provided.
    session_key = cfg.get("session")
    if session_key and session_key in SESSIONS:
        sess = SESSIONS[session_key]
        cfg["session_start"] = sess["start"]
        cfg["session_end"] = sess["end"]
        cfg["tz"] = sess["tz"]

    blueprint = cfg.get("blueprint", "ict_silver_bullet")
    if blueprint not in BLUEPRINTS:
        raise ValueError(f"Unknown blueprint: {blueprint!r}; expected one of {BLUEPRINTS}")

    dispatch = {
        "ict_silver_bullet": _ict_silver_bullet,
        "casper_inverted_fvg": _casper_inverted_fvg,
        "velez_20_200_elephant_bar": _velez_20_200_elephant_bar,
        "rosato_sd_absorption": _rosato_sd_absorption,
        "carter_ttm_squeeze": _carter_ttm_squeeze,
        "raschke_holy_grail": _raschke_holy_grail,
        "wade_pats_second_entry": _wade_pats_second_entry,
    }

    return dispatch[blueprint](df, cfg)


def _signals_from_trades(trades: List[Dict[str, Any]]) -> pd.DataFrame:
    if not trades:
        return _empty_signals()
    result = pd.DataFrame(trades)
    return result[
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


def _empty_signals() -> pd.DataFrame:
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
