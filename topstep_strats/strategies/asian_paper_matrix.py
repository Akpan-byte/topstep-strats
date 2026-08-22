# CHANGE_SUMMARY
# 2026-08-21  kilo
#   - Created topstep_strats/strategies/asian_paper_matrix.py implementing the
#     9 Asian-session engines from asian_paper.txt.
#   - Engines: Kathy Lien DBB, Rayner Teo false breakout, Tradeciety Bollinger+RSI,
#     Stacey Burke 3-push, Trading Rush HMA+MACD, Larry Williams %R,
#     UKspreadbetting Camarilla, Ezekiel Chew pin bar, Trade Prime KDJ.
#   - Built deterministic 108-row matrix (AS_001-AS_108): 9 engines x 3 instruments
#     x 2 sessions (Early Asian / Late Asian) x 2 target styles.
#   - All indicators use shift(1) / closed-bar data only; entries execute at the
#     close of the confirming bar to avoid lookahead bias.
# WHY: User requested a full Topstep-modeled backtest of the exact 108-strategy
#      matrix described in asian_paper.txt.

from __future__ import annotations

from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd

from topstep_strats import data


try:
    from numba import njit
except ImportError:  # pragma: no cover
    njit = None


# -----------------------------------------------------------------------------
# Public matrix metadata
# -----------------------------------------------------------------------------

ENGINES = [
    "kathy_lien_dbb",
    "rayner_teo_false_breakout",
    "tradeciety_bollinger_rsi",
    "stacey_burke_3push",
    "trading_rush_hma_macd",
    "larry_williams_pr",
    "ukspreadbetting_camarilla",
    "ezekiel_chew_pin_bar",
    "trade_prime_kdj",
]

INSTRUMENTS = ["ES", "NQ", "YM"]
SESSIONS = {
    "EarlyAsian": {"start": "18:00", "end": "22:00"},
    "LateAsian": {"start": "22:00", "end": "02:30"},
}
TARGET_STYLES = ["hyper_scalp", "mean_reversion"]

TICK_SIZES = {"ES": 0.25, "NQ": 0.25, "YM": 1.0}
POINT_VALUES = {"ES": 50.0, "NQ": 20.0, "YM": 5.0}


# -----------------------------------------------------------------------------
# Default parameters
# -----------------------------------------------------------------------------

def default_params() -> Dict[str, Any]:
    """Defaults shared by every Asian-paper config."""
    return {
        "engine": "kathy_lien_dbb",
        "instrument": "ES",
        "tick_size": 0.25,
        "point_value": 50.0,
        "session": "EarlyAsian",
        "session_start": "18:00",
        "session_end": "22:00",
        "tz": "America/New_York",
        "session_only": True,
        "one_trade_per_day": True,
        "target_style": "hyper_scalp",
        "tp_atr": 0.1,
        "sl_atr": 2.0,
        "atr_length": 14,
        "mode": "baseline",
    }


# -----------------------------------------------------------------------------
# 108-row matrix builder
# -----------------------------------------------------------------------------

def _build_matrix() -> List[Dict[str, Any]]:
    """Return the exact 108-row Asian-paper strategy matrix.

    Matrix = 9 engines x 3 instruments x 2 sessions x 2 target styles.
    IDs are AS_001 to AS_108.  Instrument/session are baked into each ID.
    """
    rows: List[Dict[str, Any]] = []

    # TP/SL expressed in 14-period ATR units.  Hyper-scalp = micro target,
    # mean-reversion = larger structural target.
    style_tp_sl: Dict[str, Tuple[float, float]] = {
        "hyper_scalp": (0.10, 2.0),
        "mean_reversion": (0.75, 2.0),
    }

    # Engine parameter overrides.
    engine_params: Dict[str, Dict[str, Any]] = {
        "kathy_lien_dbb": {
            "bb_length": 20,
            "outer_std": 2.0,  # NQ overridden to 2.5 below
            "inner_std": 1.0,
        },
        "rayner_teo_false_breakout": {
            "atr_length": 14,
            "breakout_range_mult": 1.5,
        },
        "tradeciety_bollinger_rsi": {
            "bb_length": 20,
            "bb_std": 2.5,
            "rsi_length": 14,
            "divergence_lookback": 10,
        },
        "stacey_burke_3push": {
            "range_minutes": 60,
            "range_ticks": 30,
            "engulfing_minutes": 5,
        },
        "trading_rush_hma_macd": {
            "hma_length": 55,
            "macd_fast": 12,
            "macd_slow": 26,
            "macd_signal": 9,
        },
        "larry_williams_pr": {
            "pr_length": 14,
            "oversold": -95,
            "overbought": -5,
            "exit_os": -80,
            "exit_ob": -20,
        },
        "ukspreadbetting_camarilla": {
            "reversal_lookback": 5,
        },
        "ezekiel_chew_pin_bar": {
            "touch_lookback": 30,
            "cluster_tick_width": 5,
            "pin_bar_lookback": 10,
        },
        "trade_prime_kdj": {
            "kdj_rsv_len": 9,
            "kdj_k_len": 3,
            "kdj_d_len": 3,
            "chop_range_ticks": 10,
            "chop_minutes": 30,
        },
    }

    counter = 1
    for engine in ENGINES:
        for instrument in INSTRUMENTS:
            for session_name, session_times in SESSIONS.items():
                for style in TARGET_STYLES:
                    sid = f"AS_{counter:03d}"
                    tp_atr, sl_atr = style_tp_sl[style]

                    cfg = default_params()
                    cfg["engine"] = engine
                    cfg["instrument"] = instrument
                    cfg["tick_size"] = TICK_SIZES[instrument]
                    cfg["point_value"] = POINT_VALUES[instrument]
                    cfg["session"] = session_name
                    cfg["session_start"] = session_times["start"]
                    cfg["session_end"] = session_times["end"]
                    cfg["target_style"] = style
                    cfg["tp_atr"] = tp_atr
                    cfg["sl_atr"] = sl_atr
                    cfg.update(engine_params[engine])

                    # Engine/instrument nuance overrides.
                    if engine == "kathy_lien_dbb" and instrument == "NQ":
                        cfg["outer_std"] = 2.5
                    if engine == "larry_williams_pr" and instrument == "YM":
                        cfg["oversold"] = -98
                        cfg["overbought"] = -2

                    # Fixed-tick hyper-scalp targets from the paper.
                    if engine == "stacey_burke_3push" and style == "hyper_scalp":
                        if instrument == "ES":
                            cfg["tp_atr"] = 1.0  # 1 point
                        else:
                            cfg["tp_atr"] = 2.5  # 2.5 points NQ/YM
                        cfg["sl_atr"] = 1.0
                    if engine == "trade_prime_kdj" and style == "hyper_scalp":
                        cfg["tp_atr"] = 1.0  # 1-2 tick scalps approximated as 1 point
                        cfg["sl_atr"] = 1.0

                    rows.append({
                        "id": sid,
                        "engine": engine,
                        "instrument": instrument,
                        "session": session_name,
                        "target_style": style,
                        "params": cfg,
                    })
                    counter += 1

    if len(rows) != 108:
        raise RuntimeError(f"Asian-paper matrix must contain exactly 108 rows, got {len(rows)}")
    return rows


_MATRIX: List[Dict[str, Any]] = _build_matrix()


# -----------------------------------------------------------------------------
# Public matrix accessors
# -----------------------------------------------------------------------------

def list_strategy_ids() -> List[str]:
    return [row["id"] for row in _MATRIX]


def get_strategy_config(sid: str) -> Dict[str, Any]:
    for row in _MATRIX:
        if row["id"] == sid:
            return row["params"].copy()
    raise ValueError(f"Unknown Asian-paper strategy ID: {sid!r}")


def get_matrix_df() -> pd.DataFrame:
    return pd.DataFrame([
        {
            "id": row["id"],
            "engine": row["engine"],
            "instrument": row["instrument"],
            "session": row["session"],
            "target_style": row["target_style"],
            **{f"param_{k}": v for k, v in row["params"].items()},
        }
        for row in _MATRIX
    ])


# -----------------------------------------------------------------------------
# Indicator helpers (closed-bar only)
# -----------------------------------------------------------------------------

def _sma(series: pd.Series, length: int) -> pd.Series:
    return series.rolling(length, min_periods=length).mean().shift(1)


def _ema(series: pd.Series, length: int) -> pd.Series:
    return series.ewm(span=length, adjust=False, min_periods=length).mean().shift(1)


def _wma(series: pd.Series, length: int) -> pd.Series:
    weights = np.arange(1, length + 1, dtype=float)
    return series.rolling(length, min_periods=length).apply(
        lambda x: np.dot(x, weights[-len(x):]) / weights[-len(x):].sum(), raw=True
    )


def _true_range(df: pd.DataFrame) -> pd.Series:
    prev_close = df["close"].shift(1)
    return pd.concat([
        df["high"] - df["low"],
        (df["high"] - prev_close).abs(),
        (df["low"] - prev_close).abs(),
    ], axis=1).max(axis=1)


def _atr(df: pd.DataFrame, length: int) -> pd.Series:
    return _true_range(df).rolling(length, min_periods=length).mean().shift(1)


def _rsi(series: pd.Series, length: int = 14) -> pd.Series:
    delta = series.diff()
    gain = delta.where(delta > 0, 0.0)
    loss = (-delta).where(delta < 0, 0.0)
    avg_gain = gain.rolling(length, min_periods=length).mean().shift(1)
    avg_loss = loss.rolling(length, min_periods=length).mean().shift(1)
    rs = avg_gain / avg_loss.replace(0, np.nan)
    rsi = 100.0 - (100.0 / (1.0 + rs))
    rsi = rsi.where(avg_loss != 0, 100.0)
    rsi = rsi.where(avg_gain != 0, 0.0)
    return rsi


def _macd(series: pd.Series, fast: int = 12, slow: int = 26, signal: int = 9) -> Tuple[pd.Series, pd.Series, pd.Series]:
    macd_line = _ema(series, fast) - _ema(series, slow)
    signal_line = _ema(macd_line, signal)
    hist = macd_line - signal_line
    return macd_line, signal_line, hist


def _hma(series: pd.Series, length: int) -> pd.Series:
    """Hull Moving Average using closed-bar data only."""
    half_len = int(length / 2)
    sqrt_len = int(np.sqrt(length))
    wma_half = _wma(series, half_len)
    wma_full = _wma(series, length)
    raw = 2.0 * wma_half - wma_full
    hma = _wma(raw, sqrt_len)
    return hma.shift(1)


def _williams_r(high: pd.Series, low: pd.Series, close: pd.Series, length: int) -> pd.Series:
    highest = high.rolling(length, min_periods=length).max().shift(1)
    lowest = low.rolling(length, min_periods=length).min().shift(1)
    range_ = highest - lowest
    return -100.0 * (highest - close) / range_.replace(0, np.nan)


def _kdj(df: pd.DataFrame, rsv_len: int, k_len: int, d_len: int) -> Tuple[pd.Series, pd.Series, pd.Series]:
    highest = df["high"].rolling(rsv_len, min_periods=rsv_len).max().shift(1)
    lowest = df["low"].rolling(rsv_len, min_periods=rsv_len).min().shift(1)
    rsv = 100.0 * (df["close"] - lowest) / (highest - lowest).replace(0, np.nan)
    k = _ema(rsv, k_len)
    d = _ema(k, d_len)
    j = 3.0 * k - 2.0 * d
    return k, d, j


def _session_date(index_utc: pd.DatetimeIndex, tz: str) -> np.ndarray:
    local = index_utc.tz_convert(tz).tz_localize(None)
    ns = local.astype("datetime64[ns]").view(np.int64)
    return (ns // 86_400_000_000_000).astype(np.int64)


def _local_time(index_utc: pd.DatetimeIndex, tz: str) -> np.ndarray:
    local = index_utc.tz_convert(tz).tz_localize(None)
    ns = local.astype("datetime64[ns]").view(np.int64)
    return (ns % 86_400_000_000_000) // 1_000_000_000


def _add_session_meta(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    tz = cfg["tz"]
    df = df.copy()
    df.attrs = {}
    df["_in_session"] = data.get_session_mask(df, cfg["session_start"], cfg["session_end"], tz)
    df["_date"] = _session_date(df.index, tz)
    df["_time_s"] = _local_time(df.index, tz)
    return df


def _empty_signals() -> pd.DataFrame:
    return pd.DataFrame(columns=["entry_time", "direction", "entry_price", "atr_value"])


def _emit_signals(trades: List[Dict[str, Any]]) -> pd.DataFrame:
    if not trades:
        return _empty_signals()
    df = pd.DataFrame(trades)
    return df[["entry_time", "direction", "entry_price", "atr_value"]]


def _entry_signal(ts: pd.Timestamp, direction: int, price: float, atr: float) -> Dict[str, Any]:
    return {"entry_time": ts, "direction": direction, "entry_price": price, "atr_value": atr}


# -----------------------------------------------------------------------------
# Signal dispatcher
# -----------------------------------------------------------------------------

def generate_signals(df_1m: pd.DataFrame, params: Dict[str, Any]) -> pd.DataFrame:
    """Generate entry-only signals for one Asian-paper strategy config."""
    if df_1m.empty:
        return _empty_signals()

    cfg = default_params()
    cfg.update(params)

    df = df_1m.copy()
    df.attrs = {}
    required = {"open", "high", "low", "close", "volume"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    df.index = df.index.tz_convert("UTC")

    engine = cfg.get("engine", "kathy_lien_dbb")
    if engine == "kathy_lien_dbb":
        return _kathy_lien_dbb(df, cfg)
    if engine == "rayner_teo_false_breakout":
        return _rayner_teo_false_breakout(df, cfg)
    if engine == "tradeciety_bollinger_rsi":
        return _tradeciety_bollinger_rsi(df, cfg)
    if engine == "stacey_burke_3push":
        return _stacey_burke_3push(df, cfg)
    if engine == "trading_rush_hma_macd":
        return _trading_rush_hma_macd(df, cfg)
    if engine == "larry_williams_pr":
        return _larry_williams_pr(df, cfg)
    if engine == "ukspreadbetting_camarilla":
        return _ukspreadbetting_camarilla(df, cfg)
    if engine == "ezekiel_chew_pin_bar":
        return _ezekiel_chew_pin_bar(df, cfg)
    if engine == "trade_prime_kdj":
        return _trade_prime_kdj(df, cfg)
    raise ValueError(f"Unknown Asian-paper engine: {engine!r}")


# -----------------------------------------------------------------------------
# Engine implementations
# -----------------------------------------------------------------------------

def _kathy_lien_dbb(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 1: Double Bollinger Band mean reversion.

    Long: prior bar wick pierces lower outer band but body closes above it;
    current (confirmation) bar closes above inner band -> buy at close.
    Short: symmetric.
    """
    length = int(cfg["bb_length"])
    outer_std = float(cfg["outer_std"])
    inner_std = float(cfg["inner_std"])

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    mid = _sma(df1["close"], length)
    std = df1["close"].rolling(length, min_periods=length).std().shift(1)
    df1["outer_upper"] = mid + outer_std * std
    df1["outer_lower"] = mid - outer_std * std
    df1["inner_upper"] = mid + inner_std * std
    df1["inner_lower"] = mid - inner_std * std

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    ou = df1["outer_upper"].values
    ol = df1["outer_lower"].values
    iu = df1["inner_upper"].values
    il = df1["inner_lower"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = in_session & ~np.isnan(ou) & ~np.isnan(ol) & ~np.isnan(iu) & ~np.isnan(il) & ~np.isnan(atr) & (atr > 0)

    # Prior bar (i-1) is the wick-pierce bar; current bar (i) is confirmation.
    prev_valid = np.roll(valid, 1)
    prev_valid[0] = False

    setup_long = prev_valid & (np.roll(low, 1) <= np.roll(ol, 1)) & (np.roll(close, 1) > np.roll(ol, 1))
    confirm_long = valid & (close > il)
    long_cond = setup_long & confirm_long

    setup_short = prev_valid & (np.roll(high, 1) >= np.roll(ou, 1)) & (np.roll(close, 1) < np.roll(ou, 1))
    confirm_short = valid & (close < iu)
    short_cond = setup_short & confirm_short

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
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr[i])))
        last_date = date

    return _emit_signals(trades)


def _rayner_teo_false_breakout(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 2: Asian range false-breakout rejection fade.

    Short: bar breaks above running Asian high with range >= 1.5 ATR, next bar
    bearish rejection (closes back below high) -> sell at close.
    Long: symmetric.
    Mean-reversion target = midpoint of Asian range.
    """
    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    open_ = df1["open"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values
    n = len(df1)

    # Running Asian high/low up to the previous bar (no lookahead).
    asian_high = np.maximum.accumulate(high)
    asian_low = np.minimum.accumulate(low)
    asian_high = np.roll(asian_high, 1)
    asian_low = np.roll(asian_low, 1)
    asian_high[0] = np.nan
    asian_low[0] = np.nan

    range_mult = float(cfg["breakout_range_mult"])
    candle_range = high - low

    valid = in_session & ~np.isnan(atr) & (atr > 0) & ~np.isnan(asian_high)
    prev_valid = np.roll(valid, 1)
    prev_valid[0] = False

    # Breakout bar = i-1, rejection bar = i.
    break_long = prev_valid & (np.roll(low, 1) < np.roll(asian_low, 1)) & (np.roll(candle_range, 1) >= range_mult * np.roll(atr, 1))
    break_short = prev_valid & (np.roll(high, 1) > np.roll(asian_high, 1)) & (np.roll(candle_range, 1) >= range_mult * np.roll(atr, 1))

    reject_long = valid & (close > asian_low) & (close > open_)
    reject_short = valid & (close < asian_high) & (close < open_)

    long_cond = break_long & reject_long
    short_cond = break_short & reject_short

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
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr[i])))
        last_date = date

    return _emit_signals(trades)


def _tradeciety_bollinger_rsi(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 3: Bollinger 2.5 SD spike + RSI reversal.

    Long: close below lower band then back inside, with RSI bullish divergence.
    Late Asian relaxes divergence to simple RSI extreme + inside close.
    """
    bb_length = int(cfg["bb_length"])
    bb_std = float(cfg["bb_std"])
    rsi_length = int(cfg["rsi_length"])
    lookback = int(cfg["divergence_lookback"])
    session = cfg.get("session", "EarlyAsian")
    late_asian = session == "LateAsian"

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    mid = _sma(df1["close"], bb_length)
    std = df1["close"].rolling(bb_length, min_periods=bb_length).std().shift(1)
    df1["upper"] = mid + bb_std * std
    df1["lower"] = mid - bb_std * std
    df1["rsi"] = _rsi(df1["close"], rsi_length)

    close = df1["close"].values
    upper = df1["upper"].values
    lower = df1["lower"].values
    rsi = df1["rsi"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = in_session & ~np.isnan(upper) & ~np.isnan(lower) & ~np.isnan(rsi) & ~np.isnan(atr) & (atr > 0)

    # Spike: prior bar closed outside band; current bar closes back inside.
    prev_close = np.roll(close, 1)
    prev_lower = np.roll(lower, 1)
    prev_upper = np.roll(upper, 1)
    spike_below = prev_close < prev_lower
    back_inside_long = valid & (close > lower) & (close < upper) & spike_below
    spike_above = prev_close > prev_upper
    back_inside_short = valid & (close < upper) & (close > lower) & spike_above

    # Divergence detection (closed bar only).
    def _has_divergence(direction: int) -> np.ndarray:
        out = np.zeros(len(df1), dtype=bool)
        for i in range(lookback + 1, len(df1)):
            win_close = close[i - lookback:i]
            win_rsi = rsi[i - lookback:i]
            if not np.any(np.isfinite(win_rsi)):
                continue
            if direction == 1:
                price_low = np.nanmin(win_close[np.isfinite(win_close)]) if np.any(np.isfinite(win_close)) else close[i]
                rsi_low = np.nanmin(win_rsi[np.isfinite(win_rsi)])
                out[i] = close[i] < price_low and rsi[i] > rsi_low
            else:
                price_high = np.nanmax(win_close[np.isfinite(win_close)]) if np.any(np.isfinite(win_close)) else close[i]
                rsi_high = np.nanmax(win_rsi[np.isfinite(win_rsi)])
                out[i] = close[i] > price_high and rsi[i] < rsi_high
        return out

    div_long = _has_divergence(1)
    div_short = _has_divergence(-1)

    if late_asian:
        long_cond = back_inside_long & (rsi > 75)
        short_cond = back_inside_short & (rsi < 25)
    else:
        long_cond = back_inside_long & div_long
        short_cond = back_inside_short & div_short

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
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr[i])))
        last_date = date

    return _emit_signals(trades)


def _stacey_burke_3push(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 4: 3-push exhaustion inside an Asian consolidation range.

    Short: 60+ min Asian range <= 30 ticks, three higher highs breaking the top,
    third-push volume declines vs first & second, subsequent 5-min bearish
    engulfing -> sell at close.
    """
    range_minutes = int(cfg["range_minutes"])
    range_ticks = float(cfg["range_ticks"])
    engulf_minutes = int(cfg["engulfing_minutes"])
    tick_size = float(cfg["tick_size"])

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    volume = df1["volume"].values.astype(float)
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values
    n = len(df1)

    range_width = range_ticks * tick_size
    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    # Precompute 5-min engulfing flags for the whole series.
    em = engulf_minutes
    body_neg = np.zeros(n, dtype=bool)
    body_pos = np.zeros(n, dtype=bool)
    body_neg[em:] = close[em:] < close[:-em]
    body_pos[em:] = close[em:] > close[:-em]

    high_em = pd.Series(high).rolling(em, min_periods=1).max().shift(1).values
    low_em = pd.Series(low).rolling(em, min_periods=1).min().shift(1).values
    high_prev_em = pd.Series(high).rolling(em, min_periods=1).max().shift(em).values
    low_prev_em = pd.Series(low).rolling(em, min_periods=1).min().shift(em).values

    bearish_engulfing = (
        body_neg & (high_em >= high_prev_em) & (low_em <= low_prev_em) & in_session
    )
    bullish_engulfing = (
        body_pos & (high_em >= high_prev_em) & (low_em <= low_prev_em) & in_session
    )

    # Precompute session boundaries using integer day labels (input may already
    # be masked to the session, so in_session is not a reliable boundary marker).
    session_start_idx = np.full(n, -1, dtype=np.int64)
    session_end_idx = np.full(n, -1, dtype=np.int64)
    current_start = -1
    current_date = -1
    for i in range(n):
        if in_session[i] and dates[i] != current_date:
            current_start = i
            current_date = dates[i]
        if in_session[i]:
            session_start_idx[i] = current_start
    # Second pass: each bar's session end is the last bar with the same start.
    for i in range(n - 1, -1, -1):
        if in_session[i]:
            if i == n - 1 or session_start_idx[i + 1] != session_start_idx[i]:
                session_end_idx[i] = i + 1
            else:
                session_end_idx[i] = session_end_idx[i + 1]

    session_starts = np.unique(session_start_idx[session_start_idx >= 0])

    # Build per-bar session range high/low arrays.
    session_range_high = np.full(n, np.nan)
    session_range_low = np.full(n, np.nan)
    for session_start in session_starts:
        session_start = int(session_start)
        session_end = int(session_end_idx[session_start])
        range_end = min(session_start + range_minutes, session_end)
        if range_end <= session_start + 5:
            continue
        rh = float(np.nanmax(high[session_start:range_end]))
        rl = float(np.nanmin(low[session_start:range_end]))
        if rh - rl > range_width:
            continue
        session_range_high[session_start:session_end] = rh
        session_range_low[session_start:session_end] = rl

    valid_range = ~np.isnan(session_range_high)

    # Vectorized peak/trough detection across the whole series.
    # A local extremum at bar k needs the next bar (k+1) to close before it
    # can be confirmed, so the confirmed flag is shifted right by one bar.
    raw_peak = np.zeros(n, dtype=bool)
    raw_peak[1:-1] = (high[1:-1] > high[:-2]) & (high[1:-1] > high[2:])
    raw_trough = np.zeros(n, dtype=bool)
    raw_trough[1:-1] = (low[1:-1] < low[:-2]) & (low[1:-1] < low[2:])

    confirmed_peak = np.zeros(n, dtype=bool)
    confirmed_peak[1:] = raw_peak[:-1]
    confirmed_trough = np.zeros(n, dtype=bool)
    confirmed_trough[1:] = raw_trough[:-1]

    peak = valid_range & confirmed_peak & (np.roll(high, 1) > session_range_high)
    trough = valid_range & confirmed_trough & (np.roll(low, 1) < session_range_low)

    peak_idx = np.where(peak)[0] - 1
    trough_idx = np.where(trough)[0] - 1

    # Group peaks/troughs by session and check setups.
    for session_start in session_starts:
        session_start = int(session_start)
        session_end = int(session_end_idx[session_start])
        if session_end < 0:
            continue

        # Short setup.
        sess_peaks = peak_idx[(peak_idx >= session_start) & (peak_idx < session_end)]
        if len(sess_peaks) >= 3:
            p1, p2, p3 = sess_peaks[-3:]
            v1, v2, v3 = volume[p1], volume[p2], volume[p3]
            if v1 > v3 and v2 > v3:
                candidates = np.where(bearish_engulfing[p3 + em:session_end])[0]
                if len(candidates) > 0:
                    i = int(candidates[0]) + p3 + em
                    date = dates[i]
                    if not (one_trade and last_date == date):
                        trades.append(_entry_signal(df1.index[i], -1, float(close[i]), float(atr[i])))
                        last_date = date

        # Long setup.
        sess_troughs = trough_idx[(trough_idx >= session_start) & (trough_idx < session_end)]
        if len(sess_troughs) >= 3:
            t1, t2, t3 = sess_troughs[-3:]
            v1, v2, v3 = volume[t1], volume[t2], volume[t3]
            if v1 > v3 and v2 > v3:
                candidates = np.where(bullish_engulfing[t3 + em:session_end])[0]
                if len(candidates) > 0:
                    i = int(candidates[0]) + t3 + em
                    date = dates[i]
                    if not (one_trade and last_date == date):
                        trades.append(_entry_signal(df1.index[i], 1, float(close[i]), float(atr[i])))
                        last_date = date

    return _emit_signals(trades)


def _trading_rush_hma_macd(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 5: HMA cross + MACD histogram flip.

    Long: close crosses above 55 HMA, MACD hist flips neg->pos, MACD line crossed
    above signal below zero axis.
    """
    hma_len = int(cfg["hma_length"])
    fast = int(cfg["macd_fast"])
    slow = int(cfg["macd_slow"])
    signal_len = int(cfg["macd_signal"])

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["hma"] = _hma(df1["close"], hma_len)
    macd_line, signal_line, hist = _macd(df1["close"], fast, slow, signal_len)
    df1["macd"] = macd_line
    df1["signal"] = signal_line
    df1["hist"] = hist
    df1["hist_prev"] = df1["hist"].shift(1)
    df1["macd_prev"] = df1["macd"].shift(1)
    df1["signal_prev"] = df1["signal"].shift(1)
    df1["close_prev"] = df1["close"].shift(1)
    df1["hma_prev"] = df1["hma"].shift(1)

    close = df1["close"].values
    hma = df1["hma"].values
    hma_prev = df1["hma_prev"].values
    close_prev = df1["close_prev"].values
    hist = df1["hist"].values
    hist_prev = df1["hist_prev"].values
    macd = df1["macd"].values
    macd_prev = df1["macd_prev"].values
    signal = df1["signal"].values
    signal_prev = df1["signal_prev"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = in_session & ~np.isnan(hma) & ~np.isnan(hist) & ~np.isnan(atr) & (atr > 0)

    long_cond = valid & (close > hma) & (close_prev <= hma_prev) & (hist > 0) & (hist_prev <= 0) & (macd > signal) & (macd_prev <= signal_prev) & (macd < 0)
    short_cond = valid & (close < hma) & (close_prev >= hma_prev) & (hist < 0) & (hist_prev >= 0) & (macd < signal) & (macd_prev >= signal_prev) & (macd > 0)

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
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr[i])))
        last_date = date

    return _emit_signals(trades)


def _larry_williams_pr(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 6: Williams %R extreme reversion.

    Long: %R drops below oversold, then crosses back above exit_os.
    Short: %R rises above overbought, then crosses back below exit_ob.
    """
    length = int(cfg["pr_length"])
    oversold = float(cfg["oversold"])
    overbought = float(cfg["overbought"])
    exit_os = float(cfg["exit_os"])
    exit_ob = float(cfg["exit_ob"])

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    df1["pr"] = _williams_r(df1["high"], df1["low"], df1["close"], length)
    df1["pr_prev"] = df1["pr"].shift(1)

    close = df1["close"].values
    pr = df1["pr"].values
    pr_prev = df1["pr_prev"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values

    valid = in_session & ~np.isnan(pr) & ~np.isnan(pr_prev) & ~np.isnan(atr) & (atr > 0)

    long_cond = valid & (pr_prev < oversold) & (pr > exit_os)
    short_cond = valid & (pr_prev > overbought) & (pr < exit_ob)

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
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr[i])))
        last_date = date

    return _emit_signals(trades)


def _ukspreadbetting_camarilla(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 7: Camarilla pivot S3/R3 reversal.

    Daily pivots are calculated from the prior trading day's OHLC.
    Short: price touches/pierces R3, then a 5-min bearish reversal pattern.
    Long: symmetric at S3.  Target is the central pivot point (PP).
    """
    rev_lookback = int(cfg["reversal_lookback"])

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values
    n = len(df1)

    # Build daily OHLC and Camarilla levels using closed-bar data only.
    local = df1.index.tz_convert("America/New_York").tz_localize(None)
    ns = local.astype("datetime64[ns]").view(np.int64)
    day_labels = (ns // 86_400_000_000_000).astype(np.int64)
    unique_days = np.unique(day_labels)
    day_index = {d: i for i, d in enumerate(unique_days)}
    day_idx_arr = np.array([day_index[d] for d in day_labels])

    daily_open = np.full(len(unique_days), np.nan)
    daily_high = np.full(len(unique_days), np.nan)
    daily_low = np.full(len(unique_days), np.nan)
    daily_close = np.full(len(unique_days), np.nan)
    for i in range(n):
        di = day_idx_arr[i]
        if np.isnan(daily_open[di]):
            daily_open[di] = close[i]
        daily_high[di] = np.nanmax([daily_high[di], high[i]])
        daily_low[di] = np.nanmin([daily_low[di], low[i]]) if not np.isnan(daily_low[di]) else low[i]
        daily_close[di] = close[i]

    pp = (daily_high + daily_low + daily_close) / 3.0
    range_ = daily_high - daily_low
    r3 = daily_close + range_ * 1.1 / 4.0
    s3 = daily_close - range_ * 1.1 / 4.0

    # Map back to minute bars (use previous day's levels for current day).
    prev_day_idx = np.maximum(day_idx_arr - 1, 0)
    r3_arr = r3[prev_day_idx]
    s3_arr = s3[prev_day_idx]
    pp_arr = pp[prev_day_idx]

    valid = in_session & ~np.isnan(r3_arr) & ~np.isnan(s3_arr) & ~np.isnan(atr) & (atr > 0)

    # Vectorized rolling window highs/lows.
    win_high = pd.Series(high).rolling(rev_lookback, min_periods=1).max().shift(1).values
    win_low = pd.Series(low).rolling(rev_lookback, min_periods=1).min().shift(1).values
    prev_win_high = pd.Series(high).rolling(rev_lookback, min_periods=1).max().shift(rev_lookback).values
    prev_win_low = pd.Series(low).rolling(rev_lookback, min_periods=1).min().shift(rev_lookback).values
    win_open = pd.Series(close).shift(rev_lookback - 1).values

    short_cond = (
        valid
        & (win_high >= r3_arr)
        & (close < win_open)
        & (win_high >= prev_win_high)
        & (win_low <= prev_win_low)
    )
    long_cond = (
        valid
        & (win_low <= s3_arr)
        & (close > win_open)
        & (win_high >= prev_win_high)
        & (win_low <= prev_win_low)
    )

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    for i in np.where(short_cond | long_cond)[0]:
        if not in_session[i]:
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue
        if short_cond[i] and long_cond[i]:
            continue
        direction = 1 if long_cond[i] else -1
        trades.append(_entry_signal(df1.index[i], direction, float(close[i]), float(atr[i])))
        last_date = date

    return _emit_signals(trades)


def _ezekiel_chew_pin_bar(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 8: Horizontal channel rejection pin bar.

    Long: support zone with 2 prior bounces (3 touchpoints), bullish pin bar
    sweeps below support but closes above.  Ignore pin bars in middle 50% of
    daily range.
    """
    lookback = int(cfg["touch_lookback"])
    cluster_ticks = float(cfg["cluster_tick_width"])
    pin_lookback = int(cfg["pin_bar_lookback"])
    tick_size = float(cfg["tick_size"])
    cluster_width = cluster_ticks * tick_size

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))

    close = df1["close"].values
    high = df1["high"].values
    low = df1["low"].values
    open_ = df1["open"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values
    n = len(df1)

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    # Vectorized precomputations.
    day_high = pd.Series(high).rolling(lookback, min_periods=1).max().shift(1).values
    day_low = pd.Series(low).rolling(lookback, min_periods=1).min().shift(1).values
    day_range = day_high - day_low
    mid_low = day_low + 0.25 * day_range
    mid_high = day_low + 0.75 * day_range

    body = np.abs(close - open_)
    lower_wick = np.minimum(open_, close) - low
    upper_wick = high - np.maximum(open_, close)

    bullish_pin = (
        in_session
        & (lower_wick >= 2.0 * body)
        & (close > open_)
        & ((close > mid_high) | (close < mid_low))
        & ~np.isnan(atr)
        & (atr > 0)
    )
    bearish_pin = (
        in_session
        & (upper_wick >= 2.0 * body)
        & (close < open_)
        & ((close > mid_high) | (close < mid_low))
        & ~np.isnan(atr)
        & (atr > 0)
    )

    for i in range(lookback + pin_lookback + 1, n):
        if not (bullish_pin[i] or bearish_pin[i]):
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue

        if bullish_pin[i]:
            prior_lows = low[i - lookback:i]
            near = np.abs(prior_lows - low[i]) <= cluster_width
            if near.sum() >= 2 and low[i] < np.median(prior_lows) and close[i] > low[i]:
                trades.append(_entry_signal(df1.index[i], 1, float(close[i]), float(atr[i])))
                last_date = date
                continue

        if bearish_pin[i]:
            prior_highs = high[i - lookback:i]
            near = np.abs(prior_highs - high[i]) <= cluster_width
            if near.sum() >= 2 and high[i] > np.median(prior_highs) and close[i] < high[i]:
                trades.append(_entry_signal(df1.index[i], -1, float(close[i]), float(atr[i])))
                last_date = date

    return _emit_signals(trades)


def _trade_prime_kdj(df: pd.DataFrame, cfg: Dict[str, Any]) -> pd.DataFrame:
    """Engine 9: KDJ micro-chop scalper.

    Long: J below 0 while price chops in 10-tick range over 30 min, then J
    crosses back above 0 and intersects K from below.
    """
    rsv_len = int(cfg["kdj_rsv_len"])
    k_len = int(cfg["kdj_k_len"])
    d_len = int(cfg["kdj_d_len"])
    chop_ticks = float(cfg["chop_range_ticks"])
    chop_minutes = int(cfg["chop_minutes"])
    tick_size = float(cfg["tick_size"])
    chop_width = chop_ticks * tick_size

    df1 = _add_session_meta(df, cfg)
    df1["atr"] = _atr(df1, int(cfg.get("atr_length", 14)))
    k, d, j = _kdj(df1, rsv_len, k_len, d_len)
    df1["k"] = k
    df1["d"] = d
    df1["j"] = j
    df1["j_prev"] = df1["j"].shift(1)
    df1["k_prev"] = df1["k"].shift(1)

    close = df1["close"].values
    j = df1["j"].values
    j_prev = df1["j_prev"].values
    k_ = df1["k"].values
    k_prev = df1["k_prev"].values
    atr = df1["atr"].values
    in_session = df1["_in_session"].values
    dates = df1["_date"].values
    n = len(df1)

    valid = in_session & ~np.isnan(j) & ~np.isnan(j_prev) & ~np.isnan(k_) & ~np.isnan(atr) & (atr > 0)

    trades: List[Dict[str, Any]] = []
    last_date: Any = None
    one_trade = cfg.get("one_trade_per_day", True)

    for i in range(chop_minutes + 1, n):
        if not valid[i]:
            continue
        date = dates[i]
        if one_trade and last_date == date:
            continue

        # Price in chop range over last chop_minutes bars.
        recent_high = float(np.nanmax(df1["high"].values[i - chop_minutes:i]))
        recent_low = float(np.nanmin(df1["low"].values[i - chop_minutes:i]))
        if recent_high - recent_low > chop_width:
            continue

        # Long: J was below 0, now above 0 and above K (cross from below).
        if j_prev[i] < 0 and j[i] > 0 and k_[i] > k_prev[i] and j[i] >= k_[i] and j_prev[i] < k_prev[i]:
            trades.append(_entry_signal(df1.index[i], 1, float(close[i]), float(atr[i])))
            last_date = date
            continue

        # Short: J was above 100, now below 100 and below K.
        if j_prev[i] > 100 and j[i] < 100 and k_[i] < k_prev[i] and j[i] <= k_[i] and j_prev[i] > k_prev[i]:
            trades.append(_entry_signal(df1.index[i], -1, float(close[i]), float(atr[i])))
            last_date = date

    return _emit_signals(trades)
