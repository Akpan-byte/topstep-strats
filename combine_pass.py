# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created combine_pass.py, a dedicated Topstep combine-pass simulator.
#   - Models 50k and 150k evaluation accounts separately from funded payouts.
#   - Tracks time-to-pass (days, trades), pass rate, failed combines, and
#     drawdown clusters under EOD or trailing daily-drawdown semantics.
# WHY: Combine evaluation is a different objective than funded-account payout
#      cadence. The user needs pass-speed statistics per strategy.

from __future__ import annotations

from typing import Any

import numpy as np
import pandas as pd


INSTRUMENTS = {
    "NQ": {"point_value": 20.0, "slippage_per_side": 0.25},
    "ES": {"point_value": 50.0, "slippage_per_side": 0.25},
    "YM": {"point_value": 5.0, "slippage_per_side": 1.00},
}

COMBINE_SPECS: dict[str, dict[str, Any]] = {
    "combine_50k": {
        "profit_target": 3000.0,
        "daily_dd": 1000.0,
        "trailing_dd": 2000.0,
        "max_contracts": 5,
        "initial_capital": 50000.0,
    },
    "combine_150k": {
        "profit_target": 9000.0,
        "daily_dd": 2000.0,
        "trailing_dd": 4500.0,
        "max_contracts": 10,
        "initial_capital": 150000.0,
    },
}


def _net_pnl(trades: pd.DataFrame, contracts: int) -> pd.Series:
    """Vectorized conversion of point PnL to net dollars after slippage + commission."""
    if "instrument" not in trades.columns:
        raise ValueError("trades DataFrame must contain an 'instrument' column")
    if "pnl" not in trades.columns:
        raise ValueError("trades DataFrame must contain a 'pnl' column")

    unknown = set(trades["instrument"].unique()) - set(INSTRUMENTS.keys())
    if unknown:
        raise ValueError(f"Unsupported instrument(s): {unknown}")

    point_values = trades["instrument"].map(lambda x: INSTRUMENTS[x]["point_value"])
    slippage_points = trades["instrument"].map(lambda x: INSTRUMENTS[x]["slippage_per_side"])

    gross = trades["pnl"] * point_values * contracts
    slippage = slippage_points * 2.0 * point_values * contracts
    commission = 5.0 * contracts
    return gross - slippage - commission


def _to_est_day(dt: pd.Series) -> pd.Series:
    """Convert a datetime series to calendar date in America/New_York."""
    if dt.dt.tz is None:
        return dt.dt.date
    return dt.dt.tz_convert("America/New_York").dt.date


def simulate_combine_passes(
    trades_df: pd.DataFrame,
    account_spec: dict[str, Any] | str,
    contracts: int = 1,
    daily_dd_mode: str = "eod",
) -> dict[str, Any]:
    """
    Simulate Topstep combine passes for a chronological trade log.

    Parameters
    ----------
    trades_df : pd.DataFrame
        Closed trades with columns: entry_time, exit_time, pnl, instrument.
    account_spec : dict or str
        Either a key from COMBINE_SPECS or a dict with required fields.
    contracts : int
        Number of contracts per trade.
    daily_dd_mode : {'eod', 'trailing'}
        'eod' measures daily drawdown from start-of-day equity.
        'trailing' measures daily drawdown from the high-water mark.

    Returns
    -------
    dict
        Pass-rate and time-to-pass statistics.
    """
    if trades_df is None or trades_df.empty:
        return {
            "pass_rate": 0.0,
            "combines_simulated": 0,
            "passed_combines": 0,
            "failed_combines": 0,
            "days_to_pass": [],
            "avg_days_to_pass": np.nan,
            "median_days_to_pass": np.nan,
            "min_days_to_pass": np.nan,
            "max_days_to_pass": np.nan,
            "p50_days_to_pass": np.nan,
            "p75_days_to_pass": np.nan,
            "p90_days_to_pass": np.nan,
            "p95_days_to_pass": np.nan,
            "p99_days_to_pass": np.nan,
            "trades_to_pass": [],
            "avg_trades_to_pass": np.nan,
            "median_trades_to_pass": np.nan,
            "avg_max_drawdown_pct": np.nan,
        }

    if isinstance(account_spec, str):
        if account_spec not in COMBINE_SPECS:
            raise ValueError(f"Unknown account spec: {account_spec}")
        spec = COMBINE_SPECS[account_spec].copy()
    else:
        spec = account_spec.copy()

    required = {"profit_target", "daily_dd", "trailing_dd", "max_contracts", "initial_capital"}
    missing = required - set(spec.keys())
    if missing:
        raise ValueError(f"account_spec missing fields: {missing}")

    if contracts > spec["max_contracts"]:
        raise ValueError(
            f"contracts ({contracts}) exceeds max_contracts ({spec['max_contracts']})"
        )

    profit_target = float(spec["profit_target"])
    daily_dd = float(spec["daily_dd"])
    trailing_dd = float(spec["trailing_dd"])
    initial_capital = float(spec["initial_capital"])

    df = trades_df.copy()
    for col in ("entry_time", "exit_time"):
        if col not in df.columns:
            raise ValueError(f"trades_df missing required column: {col}")
        df[col] = pd.to_datetime(df[col])

    df = df.sort_values("exit_time").reset_index(drop=True)
    df["net_pnl"] = _net_pnl(df, contracts)
    df["day"] = _to_est_day(df["exit_time"])

    cash = initial_capital
    high_water = initial_capital
    daily_start = initial_capital
    combine_start_day: Any = None
    daily_limit_hit = False
    current_day: Any = None
    combine_trades = 0
    combine_equities: list[float] = []

    passed_combines = 0
    failed_combines = 0
    days_to_pass: list[int] = []
    trades_to_pass: list[int] = []
    max_dd_pcts: list[float] = []

    for row in df.itertuples(index=False):
        day = row.day

        if day != current_day:
            current_day = day
            daily_start = cash
            daily_limit_hit = False
            if combine_start_day is None:
                combine_start_day = day

        if daily_limit_hit:
            continue

        trade_net = float(row.net_pnl)
        prospective_cash = cash + trade_net
        prospective_high_water = max(high_water, prospective_cash)

        # Daily drawdown check
        if daily_dd_mode == "eod":
            dd_reference = daily_start
        else:  # trailing
            dd_reference = prospective_high_water

        daily_breached = prospective_cash < dd_reference - daily_dd
        if daily_breached:
            daily_limit_hit = True
            continue

        # Trailing drawdown check = combine failure
        if prospective_cash < prospective_high_water - trailing_dd:
            failed_combines += 1
            cash = initial_capital
            high_water = initial_capital
            daily_start = initial_capital
            combine_start_day = None
            combine_trades = 0
            combine_equities = []
            continue

        # Execute trade
        cash = prospective_cash
        high_water = prospective_high_water
        combine_trades += 1
        combine_equities.append(cash)

        # Profit target reached = combine pass
        if cash >= initial_capital + profit_target:
            passed_combines += 1
            days_to_pass.append(_trading_days_between(combine_start_day, day, df["day"]))
            trades_to_pass.append(combine_trades)
            max_dd_pcts.append(_max_drawdown_pct_from_series(combine_equities))

            cash = initial_capital
            high_water = initial_capital
            daily_start = initial_capital
            combine_start_day = None
            combine_trades = 0
            combine_equities = []

    total = passed_combines + failed_combines

    def _pctile(arr, p):
        if not arr:
            return np.nan
        return float(np.percentile(arr, p))

    result = {
        "pass_rate": passed_combines / total * 100.0 if total > 0 else 0.0,
        "combines_simulated": total,
        "passed_combines": passed_combines,
        "failed_combines": failed_combines,
        "days_to_pass": days_to_pass,
        "avg_days_to_pass": float(np.mean(days_to_pass)) if days_to_pass else np.nan,
        "median_days_to_pass": float(np.median(days_to_pass)) if days_to_pass else np.nan,
        "min_days_to_pass": int(min(days_to_pass)) if days_to_pass else np.nan,
        "max_days_to_pass": int(max(days_to_pass)) if days_to_pass else np.nan,
        "p50_days_to_pass": _pctile(days_to_pass, 50),
        "p75_days_to_pass": _pctile(days_to_pass, 75),
        "p90_days_to_pass": _pctile(days_to_pass, 90),
        "p95_days_to_pass": _pctile(days_to_pass, 95),
        "p99_days_to_pass": _pctile(days_to_pass, 99),
        "trades_to_pass": trades_to_pass,
        "avg_trades_to_pass": float(np.mean(trades_to_pass)) if trades_to_pass else np.nan,
        "median_trades_to_pass": float(np.median(trades_to_pass)) if trades_to_pass else np.nan,
        "avg_max_drawdown_pct": float(np.mean(max_dd_pcts)) if max_dd_pcts else np.nan,
    }
    return result


def _trading_days_between(start_day, end_day, all_days: pd.Series) -> int:
    """Count distinct trading days between start and end inclusive."""
    start = pd.Timestamp(start_day)
    end = pd.Timestamp(end_day)
    mask = (all_days >= start.date()) & (all_days <= end.date())
    return int(all_days[mask].nunique())


def _max_drawdown_pct_from_series(equities: list[float]) -> float:
    """Peak-to-trough drawdown percent within a passing combine."""
    if not equities:
        return 0.0
    arr = np.array(equities)
    running_max = np.maximum.accumulate(arr)
    drawdowns = (running_max - arr) / running_max
    return float(drawdowns.max() * 100.0)
