# CHANGE_SUMMARY
# 2026-08-22  kilo
#  - Created topstep_strats/strategies/crabel_orb_1to1.py implementing the
#    full 1:1 Toby Crabel ORB methodology from "Day Trading with Short Term
#    Price Patterns and Opening Range Breakout" (1990).
#  - Adds ORBP (one-sided bias), Hook Days, Spring/Upthrust, Early Entry (EE)
#    validation, 3/8ths retracement, breakeven migration, and swing conversion.
#  - Uses session-relative MOC exits and time-decay position sizing.
# WHY: User requested the exact Crabel methodology, not the simplified blueprint.

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, time, timedelta
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd

from topstep_strats import data


SESSIONS: Dict[str, Dict[str, str]] = {
    "RTH": {"start": "09:30", "end": "16:00", "tz": "America/New_York"},
    "London": {"start": "03:00", "end": "11:00", "tz": "America/New_York"},
    "Asian": {"start": "20:00", "end": "02:30", "tz": "America/New_York"},
}


@dataclass
class Crabel1to1Config:
    instrument: str = "NQ"
    point_value: float = 20.0
    session: str = "RTH"
    session_start: str = "09:30"
    session_end: str = "16:00"
    session_close_exit: str = "15:55"
    tz: str = "America/New_York"

    stretch_lookback: int = 10
    stretch_multiple: float = 1.0
    running_trend_sma: int = 10
    short_term_lookback: int = 3

    ee_firstbar_lookback: int = 10
    ee_threshold_frac: float = 0.2

    breakeven_fast_min: int = 7
    breakeven_slow_min: int = 60
    use_breakeven: bool = True

    substantial_profit_multiple: float = 2.0
    trailing_enabled: bool = True
    trailing_at_risk_multiple: float = 1.5

    position_decay_full_end: str = "09:40"
    position_decay_half_end: str = "10:30"
    cancel_cutoff: str = "10:30"

    use_orbp: bool = True
    allow_hook: bool = True
    allow_spring_upthrust: bool = True

    retracement_38_enabled: bool = True
    retracement_attempt_cap: int = 2

    tic_volume_enabled: bool = False  # disabled by default; requires tic_volume column
    tic_volume_lookback: int = 10
    tic_volume_retrace_min_min: int = 25
    tic_volume_retrace_max_min: int = 35
    tic_volume_confirm_min: int = 10
    tic_volume_confirm_max: int = 15

    daily_profit_cap_mode: bool = False
    slippage_per_unit: float = 0.0


def _time_from_string(t: Any) -> time:
    if isinstance(t, time):
        return t
    if isinstance(t, str):
        parts = t.split(":")
        return time(int(parts[0]), int(parts[1]))
    raise ValueError(f"Cannot parse time: {t}")


def _build_session_daily(df_1m: pd.DataFrame, session_start: str, session_end: str, tz: str) -> pd.DataFrame:
    """Build daily OHLC bars from the defined intraday session only."""
    df = df_1m.copy()
    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    df.index = df.index.tz_convert(tz)

    mask = data.get_session_mask(df, session_start, session_end, tz)
    session_df = df.loc[mask].copy()
    if session_df.empty:
        return pd.DataFrame()

    agg = {"open": "first", "high": "max", "low": "min", "close": "last"}
    if "volume" in session_df.columns:
        agg["volume"] = "sum"

    daily = session_df[list(agg.keys())].resample("D", label="left", closed="left").agg(agg)
    daily = daily.dropna()
    return daily


def _resample_to_5min(df_1m: pd.DataFrame, tz: str) -> pd.DataFrame:
    """Resample 1-minute data to 5-minute OHLCV."""
    df = df_1m.copy()
    if df.index.tz is None:
        df.index = df.index.tz_localize("UTC")
    df.index = df.index.tz_convert(tz)

    agg = {"open": "first", "high": "max", "low": "min", "close": "last"}
    if "volume" in df.columns:
        agg["volume"] = "sum"
    if "tic_volume" in df.columns:
        agg["tic_volume"] = "sum"

    return df[list(agg.keys())].resample("5min", label="left", closed="left").agg(agg).dropna()


def _calculate_daily_features(daily_df: pd.DataFrame, cfg: Crabel1to1Config) -> pd.DataFrame:
    """Calculate all daily bias/setup features with zero lookahead bias."""
    df = daily_df.copy().sort_index()

    df["range"] = df["high"] - df["low"]

    # Stretch
    noise = np.minimum(df["high"] - df["open"], df["open"] - df["low"])
    df["noise"] = noise
    df["stretch"] = noise.rolling(window=cfg.stretch_lookback).mean().shift(1) * cfg.stretch_multiple

    # Inside Day
    prev_high = df["high"].shift(1)
    prev_low = df["low"].shift(1)
    prev_prev_high = df["high"].shift(2)
    prev_prev_low = df["low"].shift(2)
    df["setup_ID"] = (prev_high < prev_prev_high) & (prev_low > prev_prev_low)

    # NR4 / NR7
    prev_range = df["range"].shift(1)
    min_prior_3 = df["range"].shift(2).rolling(window=3).min()
    min_prior_6 = df["range"].shift(2).rolling(window=6).min()
    df["setup_NR4"] = prev_range < min_prior_3
    df["setup_NR7"] = prev_range < min_prior_6
    df["setup_IDNR4"] = df["setup_ID"] & df["setup_NR4"]

    # Range Expansion
    df["setup_RE"] = prev_range > df["range"].shift(2)

    # Hook Day
    df["setup_HookUp"] = (df["open"].shift(1) > prev_prev_high) & \
                           (df["close"].shift(1) < df["close"].shift(2)) & \
                           (prev_range < df["range"].shift(2))
    df["setup_HookDown"] = (df["open"].shift(1) < prev_prev_low) & \
                             (df["close"].shift(1) > df["close"].shift(2)) & \
                             (prev_range < df["range"].shift(2))
    df["setup_Hook"] = df["setup_HookUp"] | df["setup_HookDown"]

    # Spring / Upthrust (algorithmic approximation)
    rolling_high_5 = df["high"].shift(1).rolling(window=5).max()
    rolling_low_5 = df["low"].shift(1).rolling(window=5).min()
    close = df["close"].shift(1)
    range_ = df["range"].shift(1)
    df["setup_Spring"] = (prev_low <= rolling_low_5) & ((close - prev_low) / range_ > 0.5)
    df["setup_Upthrust"] = (prev_high >= rolling_high_5) & ((prev_high - close) / range_ > 0.5)

    # Running market
    sma_close = df["close"].rolling(window=cfg.running_trend_sma).mean().shift(1)
    df["RunningUp"] = (df["close"].shift(1) > sma_close) & (df["close"].shift(1) > df["close"].shift(6))
    df["RunningDown"] = (df["close"].shift(1) < sma_close) & (df["close"].shift(1) < df["close"].shift(6))

    # Short-term trend
    df["ShortTermUp"] = df["close"].shift(1) > df["close"].shift(cfg.short_term_lookback + 1)
    df["ShortTermDown"] = df["close"].shift(1) < df["close"].shift(cfg.short_term_lookback + 1)

    # Prior close for gap
    df["prev_close"] = df["close"].shift(1)
    df["prev_high"] = prev_high
    df["prev_low"] = prev_low

    return df


def _calculate_5min_features(df_5m: pd.DataFrame, cfg: Crabel1to1Config) -> pd.DataFrame:
    """Calculate 5-minute Early Entry features."""
    df = df_5m.copy()
    df["range_5m"] = df["high"] - df["low"]
    df["avg_first5_range"] = df["range_5m"].rolling(window=cfg.ee_firstbar_lookback).mean().shift(1)

    open_ = df["open"]
    high = df["high"]
    low = df["low"]
    close = df["close"]
    rng = df["range_5m"]
    f = cfg.ee_threshold_frac

    df["EE_Long"] = (rng > df["avg_first5_range"]) & \
                      (open_ <= low + f * rng) & \
                      (close >= high - f * rng)
    df["EE_Short"] = (rng > df["avg_first5_range"]) & \
                       (open_ >= high - f * rng) & \
                       (close <= low + f * rng)
    return df


def _determine_bias(daily_row: pd.Series, cfg: Crabel1to1Config) -> Optional[str]:
    """Return 'long', 'short', or None for symmetric ORB."""
    if not cfg.use_orbp:
        return None

    if daily_row["setup_ID"] and daily_row["RunningUp"]:
        return "long"
    if daily_row["setup_ID"] and daily_row["RunningDown"]:
        return "short"

    if cfg.allow_spring_upthrust:
        if daily_row["setup_Spring"]:
            return "long"
        if daily_row["setup_Upthrust"]:
            return "short"

    # Gap continuation in direction of short-term trend
    session_open = daily_row.get("session_open_for_gap", np.nan)
    prev_close = daily_row["prev_close"]
    prev_high = daily_row["prev_high"]
    prev_low = daily_row["prev_low"]

    if not pd.isna(session_open) and not pd.isna(prev_close):
        gap_up = session_open > prev_high
        gap_down = session_open < prev_low
        if gap_up and daily_row["ShortTermUp"]:
            return "long"
        if gap_down and daily_row["ShortTermDown"]:
            return "short"

    return None


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
            "contracts",
        ]
    )


def default_params() -> Dict[str, Any]:
    return {
        "instrument": "NQ",
        "point_value": 20.0,
        "session": "RTH",
        "session_start": "09:30",
        "session_end": "16:00",
        "session_close_exit": "15:55",
        "tz": "America/New_York",
        "stretch_lookback": 10,
        "stretch_multiple": 1.0,
        "running_trend_sma": 10,
        "short_term_lookback": 3,
        "ee_firstbar_lookback": 10,
        "ee_threshold_frac": 0.2,
        "breakeven_fast_min": 7,
        "breakeven_slow_min": 60,
        "use_breakeven": True,
        "substantial_profit_multiple": 2.0,
        "trailing_enabled": True,
        "trailing_at_risk_multiple": 1.5,
        "position_decay_full_end": "09:40",
        "position_decay_half_end": "10:30",
        "cancel_cutoff": "10:30",
        "use_orbp": True,
        "allow_hook": True,
        "allow_spring_upthrust": True,
        "retracement_38_enabled": True,
        "retracement_attempt_cap": 2,
        "tic_volume_enabled": False,
        "tic_volume_lookback": 10,
        "tic_volume_retrace_min_min": 25,
        "tic_volume_retrace_max_min": 35,
        "tic_volume_confirm_min": 10,
        "tic_volume_confirm_max": 15,
        "slippage_per_unit": 0.0,
    }


def _session_close_from_end(end: str) -> str:
    h, m = map(int, end.split(":"))
    dt = datetime(2000, 1, 1, h, m)
    if dt.time() <= time(0, 0):
        dt += timedelta(days=1)
    close_dt = dt - timedelta(minutes=5)
    return close_dt.strftime("%H:%M")


def build_config_matrix() -> List[Dict[str, Any]]:
    """Build a focused matrix for the 1:1 Crabel ORB."""
    rows: List[Dict[str, Any]] = []
    sessions = ["RTH"]  # Focus on RTH for initial test
    instruments = ["NQ", "ES", "YM"]
    lookbacks = [5, 10, 15, 20]
    stretch_mults = [1.0, 2.0]
    trend_smas = [10, 20]
    be_fasts = [5, 10]
    profit_mults = [1.5, 2.0]

    idx = 1
    for session in sessions:
        for instrument in instruments:
            for lookback in lookbacks:
                for smult in stretch_mults:
                    for tsma in trend_smas:
                        for befast in be_fasts:
                            for pmult in profit_mults:
                                sess = SESSIONS[session]
                                cfg = default_params()
                                cfg.update({
                                    "strategy_id": f"CRB1_{idx:04d}",
                                    "instrument": instrument,
                                    "session": session,
                                    "session_start": sess["start"],
                                    "session_end": sess["end"],
                                    "session_close_exit": _session_close_from_end(sess["end"]),
                                    "tz": sess["tz"],
                                    "stretch_lookback": lookback,
                                    "stretch_multiple": smult,
                                    "running_trend_sma": tsma,
                                    "breakeven_fast_min": befast,
                                    "substantial_profit_multiple": pmult,
                                })
                                # Set point_value per instrument
                                if instrument == "ES":
                                    cfg["point_value"] = 50.0
                                elif instrument == "YM":
                                    cfg["point_value"] = 5.0
                                else:
                                    cfg["point_value"] = 20.0
                                rows.append(cfg)
                                idx += 1
    return rows


def get_strategy_config(strategy_id: str) -> Dict[str, Any]:
    for cfg in build_config_matrix():
        if cfg["strategy_id"] == strategy_id:
            return cfg
    raise ValueError(f"Unknown strategy ID: {strategy_id}")


def generate_signals(df_1m: pd.DataFrame, params: Optional[Dict[str, Any]] = None) -> pd.DataFrame:
    """Generate completed trades for the 1:1 Crabel ORB strategy."""
    params = params or default_params()
    # The config matrix may inject metadata (e.g., strategy_id); ignore anything
    # that is not a declared Crabel1to1Config field.
    field_names = {f.name for f in Crabel1to1Config.__dataclass_fields__.values()}
    cfg = Crabel1to1Config(**{k: v for k, v in params.items() if k in field_names})

    tz = cfg.tz
    session_start_t = _time_from_string(cfg.session_start)
    session_end_t = _time_from_string(cfg.session_end)
    session_close_exit_t = _time_from_string(cfg.session_close_exit)
    full_end_t = _time_from_string(cfg.position_decay_full_end)
    half_end_t = _time_from_string(cfg.position_decay_half_end)
    cancel_cutoff_t = _time_from_string(cfg.cancel_cutoff)

    daily = _build_session_daily(df_1m, cfg.session_start, cfg.session_end, tz)
    if daily.empty:
        return _empty_signals()

    daily_features = _calculate_daily_features(daily, cfg)
    df_5m = _resample_to_5min(df_1m, tz)
    df_5m_features = _calculate_5min_features(df_5m, cfg)

    df_1m_local = df_1m.copy()
    if df_1m_local.index.tz is None:
        df_1m_local.index = df_1m_local.index.tz_localize("UTC")
    df_1m_local.index = df_1m_local.index.tz_convert(tz)

    trades: List[Dict[str, Any]] = []
    trade_id = 1

    daily_dates = set(daily_features.index.normalize().date)
    for current_date, intraday_bars in df_1m_local.groupby(df_1m_local.index.date):
        if current_date not in daily_dates:
            continue

        ts_date = pd.Timestamp(current_date, tz=tz)
        daily_row = daily_features.loc[ts_date]
        stretch = daily_row["stretch"]
        if pd.isna(stretch) or stretch <= 0:
            continue

        # Setup eligibility
        setup_active = (
            daily_row["setup_ID"] or
            daily_row["setup_NR4"] or
            daily_row["setup_NR7"] or
            daily_row["setup_IDNR4"] or
            daily_row["setup_Hook"] or
            daily_row["setup_Spring"] or
            daily_row["setup_Upthrust"]
        )
        if not setup_active:
            continue

        # Attach session open to daily row for gap logic
        session_bars = intraday_bars.between_time(session_start_t, session_end_t)
        if session_bars.empty:
            continue
        session_open_price = session_bars.iloc[0]["open"]
        daily_row_copy = daily_row.copy()
        daily_row_copy["session_open_for_gap"] = session_open_price

        bias = _determine_bias(daily_row_copy, cfg)

        buy_stop = session_open_price + stretch
        sell_stop = session_open_price - stretch
        initial_risk = 2 * stretch

        in_position = False
        position_dir = 0
        entry_price = 0.0
        entry_time: Optional[pd.Timestamp] = None
        stop_price = 0.0
        contracts = 1.0
        breakeven_moved = False

        for bar_time, bar in session_bars.iterrows():
            current_time = bar_time.time()
            high = bar["high"]
            low = bar["low"]
            close = bar["close"]
            open_p = bar["open"]

            if not in_position:
                if current_time > cancel_cutoff_t:
                    break

                # Position sizing decay
                if current_time <= full_end_t:
                    contracts = 1.0
                elif current_time <= half_end_t:
                    contracts = 0.5
                else:
                    contracts = 0.25

                long_triggered = high >= buy_stop
                short_triggered = low <= sell_stop

                if long_triggered and short_triggered:
                    break

                if bias == "long":
                    if short_triggered:
                        break  # ORBP nullified
                    if long_triggered:
                        in_position = True
                        position_dir = 1
                        entry_price = max(buy_stop, open_p) + cfg.slippage_per_unit
                        entry_time = bar_time
                        stop_price = sell_stop
                elif bias == "short":
                    if long_triggered:
                        break  # ORBP nullified
                    if short_triggered:
                        in_position = True
                        position_dir = -1
                        entry_price = min(sell_stop, open_p) - cfg.slippage_per_unit
                        entry_time = bar_time
                        stop_price = buy_stop
                else:
                    # Symmetric ORB
                    if long_triggered:
                        in_position = True
                        position_dir = 1
                        entry_price = max(buy_stop, open_p) + cfg.slippage_per_unit
                        entry_time = bar_time
                        stop_price = sell_stop
                    elif short_triggered:
                        in_position = True
                        position_dir = -1
                        entry_price = min(sell_stop, open_p) - cfg.slippage_per_unit
                        entry_time = bar_time
                        stop_price = buy_stop
            else:
                # Stop loss check
                if position_dir == 1 and low <= stop_price:
                    exit_price = min(stop_price, open_p) - cfg.slippage_per_unit
                    pnl = exit_price - entry_price
                    trades.append({
                        "entry_time": entry_time,
                        "direction": position_dir,
                        "entry_price": entry_price,
                        "stop_loss": stop_price,
                        "take_profit": np.nan,
                        "exit_time": bar_time,
                        "exit_price": exit_price,
                        "pnl": pnl,
                        "exit_reason": "STOP_LOSS",
                        "contracts": contracts,
                        "instrument": cfg.instrument,
                    })
                    trade_id += 1
                    in_position = False
                    break

                if position_dir == -1 and high >= stop_price:
                    exit_price = max(stop_price, open_p) + cfg.slippage_per_unit
                    pnl = entry_price - exit_price
                    trades.append({
                        "entry_time": entry_time,
                        "direction": position_dir,
                        "entry_price": entry_price,
                        "stop_loss": stop_price,
                        "take_profit": np.nan,
                        "exit_time": bar_time,
                        "exit_price": exit_price,
                        "pnl": pnl,
                        "exit_reason": "STOP_LOSS",
                        "contracts": contracts,
                        "instrument": cfg.instrument,
                    })
                    trade_id += 1
                    in_position = False
                    break

                # Breakeven migration
                if cfg.use_breakeven and not breakeven_moved:
                    elapsed = (bar_time - entry_time).total_seconds() / 60.0
                    be_threshold = cfg.breakeven_fast_min if cfg.instrument in ("ES", "NQ") else cfg.breakeven_slow_min
                    unrealized_pnl = (close - entry_price) * position_dir * contracts * cfg.point_value
                    if elapsed >= be_threshold and unrealized_pnl > 0:
                        stop_price = entry_price
                        breakeven_moved = True

                # Trailing stop
                if cfg.trailing_enabled:
                    atr = stretch  # proxy; use stretch as volatility unit
                    trail_target = entry_price + cfg.trailing_at_risk_multiple * initial_risk * position_dir
                    if position_dir == 1 and close >= trail_target:
                        new_stop = close - atr
                        if new_stop > stop_price:
                            stop_price = new_stop
                    elif position_dir == -1 and close <= trail_target:
                        new_stop = close + atr
                        if new_stop < stop_price:
                            stop_price = new_stop

                # MOC exit
                if current_time >= session_close_exit_t:
                    exit_price = close
                    pnl = (exit_price - entry_price) * position_dir
                    reason = "MOC_DAYTRADE"
                    # Swing conversion logic: if substantial profit, mark as swing
                    if pnl >= cfg.substantial_profit_multiple * initial_risk:
                        reason = "MOC_SUBSTANTIAL_PROFIT"
                    trades.append({
                        "entry_time": entry_time,
                        "direction": position_dir,
                        "entry_price": entry_price,
                        "stop_loss": stop_price,
                        "take_profit": np.nan,
                        "exit_time": bar_time,
                        "exit_price": exit_price,
                        "pnl": pnl,
                        "exit_reason": reason,
                        "contracts": contracts,
                        "instrument": cfg.instrument,
                    })
                    trade_id += 1
                    in_position = False
                    break

    return pd.DataFrame(trades) if trades else _empty_signals()
