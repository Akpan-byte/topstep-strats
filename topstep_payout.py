# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created topstep_payout.py, a Topstep combine/prop payout simulator.
#   - Supports 50k/150k standard and consistency account specs.
#   - Models NQ/ES/YM point values, round-turn slippage, per-contract commission.
#   - Enforces daily drawdown, trailing drawdown, profit-target payout,
#     winning-day minimums, 40%% consistency rule, and optional daily profit cap.
#   - Provides simulate_topstep_payouts(trades_df, account_spec, contracts=1,
#     daily_profit_cap_mode=False) helper.
# WHY: User needs a deterministic, fast simulator to estimate payout cadence
#      and risk-of-reset for futures strategies before running full backtests.

from __future__ import annotations

from typing import Any

import numpy as np
import pandas as pd


# ---------------------------------------------------------------------------
# Instrument definitions
# ---------------------------------------------------------------------------
INSTRUMENTS = {
    "NQ": {"point_value": 20.0, "slippage_per_side": 0.25},
    "ES": {"point_value": 50.0, "slippage_per_side": 0.25},
    "YM": {"point_value": 5.0, "slippage_per_side": 1.00},
}


# ---------------------------------------------------------------------------
# Account specs
# ---------------------------------------------------------------------------
TOPSTEP_SPECS: dict[str, dict[str, Any]] = {
    "50k_standard": {
        "profit_target": 3000,
        "daily_dd": 1000,
        "trailing_dd": 2000,
        "max_contracts": 2,
        "payout_cap": 4000,
        "min_winning_days": 5,
        "consistency_rule": True,
        "payout_pct_of_balance": 0.40,
        "initial_capital": 50000,
    },
    "50k_consistency": {
        "profit_target": 3000,
        "daily_dd": 1000,
        "trailing_dd": 2000,
        "max_contracts": 2,
        "payout_cap": 6000,
        "min_winning_days": 3,
        "consistency_rule": True,
        "payout_pct_of_balance": 0.40,
        "initial_capital": 50000,
    },
    "150k_standard": {
        "profit_target": 9000,
        "daily_dd": 2000,
        "trailing_dd": 4500,
        "max_contracts": 3,
        "payout_cap": 10000,
        "min_winning_days": 5,
        "consistency_rule": True,
        "payout_pct_of_balance": 0.40,
        "initial_capital": 150000,
    },
    "150k_consistency": {
        "profit_target": 9000,
        "daily_dd": 2000,
        "trailing_dd": 4500,
        "max_contracts": 3,
        "payout_cap": 12000,
        "min_winning_days": 3,
        "consistency_rule": True,
        "payout_pct_of_balance": 0.40,
        "initial_capital": 150000,
    },
}


def _resolve_account_spec(account_spec: dict[str, Any] | str) -> dict[str, Any]:
    """Return a concrete account spec dict, deriving initial_capital if absent."""
    if isinstance(account_spec, str):
        if account_spec not in TOPSTEP_SPECS:
            raise ValueError(f"Unknown account spec: {account_spec}")
        return TOPSTEP_SPECS[account_spec].copy()

    spec = account_spec.copy()
    if "initial_capital" not in spec:
        # Derive from account size hints in the spec name or keys.
        name = str(spec.get("name", "")).lower()
        if "150k" in name or spec.get("profit_target") == 9000:
            spec["initial_capital"] = 150000
        elif "50k" in name or spec.get("profit_target") == 3000:
            spec["initial_capital"] = 50000
        else:
            raise ValueError(
                "account_spec must include 'initial_capital' or a recognizable "
                "Topstep size (50k/150k)."
            )
    return spec


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
    slippage = slippage_points * 2.0 * point_values * contracts  # round-turn
    commission = 5.0 * contracts

    return gross - slippage - commission


def _to_est_day(dt: pd.Series) -> pd.Series:
    """Convert a datetime series to calendar date in America/New_York."""
    if dt.dt.tz is None:
        # Naive input is treated as EST for deterministic backtest behavior.
        return dt.dt.date
    return dt.dt.tz_convert("America/New_York").dt.date


def simulate_topstep_payouts(
    trades_df: pd.DataFrame,
    account_spec: dict[str, Any] | str,
    contracts: int = 1,
    daily_profit_cap_mode: bool = False,
) -> dict[str, Any]:
    """
    Simulate Topstep combine/prop payouts for a chronological trade log.

    Parameters
    ----------
    trades_df : pd.DataFrame
        Closed trades with columns:
        - entry_time : datetime
        - exit_time  : datetime
        - pnl        : float, points per contract
        - instrument : str, one of 'NQ', 'ES', 'YM'
    account_spec : dict or str
        Either a key from TOPSTEP_SPECS (e.g. '50k_standard') or a dict with
        the required fields.  Must include 'initial_capital' unless it can be
        inferred from the profit_target or name.
    contracts : int, default 1
        Number of contracts traded per signal.
    daily_profit_cap_mode : bool, default False
        If True, blocks any trade that would push the day's running net PnL
        above 25% of the profit target.  Typical for funded/payout runs.

    Returns
    -------
    dict
        Simulation summary.
    """
    if trades_df is None or trades_df.empty:
        raise ValueError("trades_df must not be empty")

    spec = _resolve_account_spec(account_spec)
    required = {
        "profit_target",
        "daily_dd",
        "trailing_dd",
        "max_contracts",
        "payout_cap",
        "min_winning_days",
        "consistency_rule",
        "payout_pct_of_balance",
        "initial_capital",
    }
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
    payout_cap = float(spec["payout_cap"])
    min_winning_days = int(spec["min_winning_days"])
    consistency_rule = bool(spec["consistency_rule"])
    payout_pct = float(spec["payout_pct_of_balance"])
    initial_capital = float(spec["initial_capital"])

    # -----------------------------------------------------------------------
    # Prepare trades
    # -----------------------------------------------------------------------
    df = trades_df.copy()
    for col in ("entry_time", "exit_time"):
        if col not in df.columns:
            raise ValueError(f"trades_df missing required column: {col}")
        df[col] = pd.to_datetime(df[col])

    df = df.sort_values("exit_time").reset_index(drop=True)
    df["net_pnl"] = _net_pnl(df, contracts)
    df["day"] = _to_est_day(df["exit_time"])

    # -----------------------------------------------------------------------
    # Simulation state
    # -----------------------------------------------------------------------
    cash = initial_capital
    high_water = initial_capital
    daily_start = initial_capital
    daily_pnl = 0.0
    total_pnl_combine = 0.0

    winning_days_current = 0
    day_pnl_current: dict[Any, float] = {}

    resets = 0
    failed_combines = 0
    payouts_count = 0
    daily_limit_hits = 0
    profit_cap_blocks = 0
    consistency_violations_total = 0
    winning_days_total = 0
    total_payouts_dollars = 0.0

    trades_executed = 0
    winning_trades = 0
    total_trade_pnl = 0.0
    max_drawdown_pct = 0.0

    trading_days: set[Any] = set()
    daily_equity: dict[Any, float] = {}

    prev_day = None

    def _reset_account() -> None:
        nonlocal cash, high_water, daily_start, daily_pnl, total_pnl_combine
        nonlocal winning_days_current, day_pnl_current
        cash = initial_capital
        high_water = initial_capital
        daily_start = initial_capital
        daily_pnl = 0.0
        total_pnl_combine = 0.0
        winning_days_current = 0
        day_pnl_current = {}

    def _consistency_violated() -> bool:
        """True if any single day in the current combine exceeds 40% of total PnL."""
        if not consistency_rule or total_pnl_combine <= 0:
            return False
        threshold = 0.4 * total_pnl_combine
        return any(dpnl > threshold for dpnl in day_pnl_current.values())

    def _finalize_day(day: Any) -> None:
        nonlocal cash, high_water, daily_start, daily_pnl, total_pnl_combine
        nonlocal winning_days_current, winning_days_total
        nonlocal consistency_violations_total
        nonlocal total_payouts_dollars, payouts_count, resets

        if daily_pnl > 0:
            winning_days_current += 1
            winning_days_total += 1

        day_pnl_current[day] = daily_pnl

        # Payout eligibility
        if (
            cash >= initial_capital + profit_target
            and winning_days_current >= min_winning_days
            and not _consistency_violated()
        ):
            withdrawal = min(cash * payout_pct, payout_cap)
            total_payouts_dollars += withdrawal
            cash -= withdrawal
            payouts_count += 1
            resets += 1
            _reset_account()
        elif (
            consistency_rule
            and cash >= initial_capital + profit_target
            and winning_days_current >= min_winning_days
            and _consistency_violated()
        ):
            # Track that a payout was blocked by the consistency rule.
            consistency_violations_total += 1

        daily_equity[day] = cash

    # -----------------------------------------------------------------------
    # Walk trades chronologically
    # -----------------------------------------------------------------------
    for row in df.itertuples(index=False):
        day = row.day

        if day != prev_day:
            if prev_day is not None:
                _finalize_day(prev_day)
            daily_start = cash
            daily_pnl = 0.0
            prev_day = day

        trade_net = float(row.net_pnl)

        # Daily profit cap
        if daily_profit_cap_mode and daily_pnl + trade_net > profit_target * 0.25:
            profit_cap_blocks += 1
            continue

        # Daily drawdown limit
        if cash + trade_net < daily_start - daily_dd:
            daily_limit_hits += 1
            continue

        # Execute trade
        cash += trade_net
        daily_pnl += trade_net
        total_pnl_combine += trade_net
        total_trade_pnl += trade_net
        trades_executed += 1
        trading_days.add(day)

        if trade_net > 0:
            winning_trades += 1

        if cash > high_water:
            high_water = cash

        dd_pct = (high_water - cash) / high_water if high_water > 0 else 0.0
        if dd_pct > max_drawdown_pct:
            max_drawdown_pct = dd_pct

        # Trailing drawdown -> reset (buy a new combine)
        if cash < high_water - trailing_dd:
            resets += 1
            failed_combines += 1
            _reset_account()

    # Finalize last day
    if prev_day is not None:
        _finalize_day(prev_day)

    # -----------------------------------------------------------------------
    # Output metrics
    # -----------------------------------------------------------------------
    trading_days_count = len(trading_days)
    total_pnl_overall = total_trade_pnl

    result = {
        "total_payouts_dollars": total_payouts_dollars,
        "avg_payout_per_week": (
            total_payouts_dollars / (trading_days_count / 5.0)
            if trading_days_count > 0
            else 0.0
        ),
        "avg_payout_per_month": (
            total_payouts_dollars / (trading_days_count / 21.0)
            if trading_days_count > 0
            else 0.0
        ),
        "resets": resets,
        "failed_combines": failed_combines,
        "daily_limit_hits": daily_limit_hits,
        "profit_cap_blocks": profit_cap_blocks,
        "winning_days": winning_days_total,
        "consistency_violations": consistency_violations_total,
        "payouts_count": payouts_count,
        "final_equity": cash,
        "trades_executed": trades_executed,
        "win_rate": (winning_trades / trades_executed * 100.0) if trades_executed > 0 else 0.0,
        "avg_trade_pnl": (total_trade_pnl / trades_executed) if trades_executed > 0 else 0.0,
        "max_drawdown_pct": max_drawdown_pct * 100.0,
        "avg_daily_profit": (
            total_pnl_overall / trading_days_count if trading_days_count > 0 else 0.0
        ),
        "daily_equity": pd.Series(daily_equity).sort_index(),
    }

    return result
