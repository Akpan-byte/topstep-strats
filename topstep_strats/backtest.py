# CHANGE_SUMMARY
# 2026-07-25  backtest-agent
#   - Implemented run_backtest(signals_df, params=None) with round-turn
#     cost handling, equity-curve construction, daily returns, and
#     start-of-day-to-trough drawdown tracking.
# 2026-07-25  kilo
#   - Added optional Topstep combine-rule enforcement:
#     * $900 daily start-of-day to trough drawdown limit
#     * $2,000 trailing peak-to-trough drawdown limit
#     * $3,000 profit target for a $50k account
#   - Reports daily-limit hits, trailing-limit failures, profit-target
#     reached, max/avg daily drawdown, and peak-to-trough drawdown.
# 2026-07-25  kilo
#   - Fixed Topstep rule semantics for ongoing performance evaluation:
#     * hitting the profit target now only sets a flag and does NOT stop
#       trading for the rest of the backtest;
#     * the daily drawdown limit now skips only the remainder of the
#       current session and resets the next day;
#     * the trailing drawdown limit remains a permanent account failure.
# 2026-08-19  kilo
#   - Added hold-duration statistics to the summary: avg_hold_seconds,
#     median_hold_seconds, p10_hold_seconds, and p90_hold_seconds, computed
#     from exit_time - entry_time of executed trades.
# WHY: Runner sweeps need to flag trades with unrealistically short hold
#      times (e.g. <5 seconds) caused by simulation artifacts.
# 2026-07-25  kilo
# WHY: The combine profit target is a pass/fail gate, not a daily trading
#      halt. For payout/evaluation the strategy must keep trading every day
#      while still respecting the $900 daily and $2k trailing loss limits.

from __future__ import annotations

from typing import Any, Dict, List

import numpy as np
import pandas as pd


REQUIRED_COLS = [
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


def default_params() -> Dict[str, Any]:
    """Default backtest parameters.

    Costs are expressed in NQ points per round-turn trade.  slippage and
    commission are subtracted from the raw per-trade ``pnl``.

    Topstep combine rules are disabled by default so the same engine can
    report raw strategy performance and rule-constrained performance.
    """
    return {
        "initial_capital": 100_000.0,
        "point_value": 20.0,        # CME E-mini NQ is $20 per point
        "slippage": 0.0,            # points, round-turn
        "commission": 0.0,          # points, round-turn
        "topstep": {
            "enabled": False,
            "account_size": 50_000.0,
            "daily_drawdown_limit": 900.0,
            "trailing_drawdown_limit": 2_000.0,
            "profit_target": 3_000.0,
        },
    }


def run_backtest(signals_df: pd.DataFrame, params: Dict[str, Any] | None = None) -> Dict[str, Any]:
    """Run a vectorized backtest on a signal table.

    Parameters
    ----------
    signals_df : pd.DataFrame
        Must contain columns: entry_time, direction, entry_price,
        stop_loss, take_profit, exit_time, exit_price, pnl, exit_reason.
        ``pnl`` is expected in NQ points.
    params : dict, optional
        Overrides for default_params().

    Returns
    -------
    dict
        trades : pd.DataFrame
            Copy of ``signals_df`` augmented with net_pnl, cost,
            trade_pnl_dollars, equity, and rule-status columns.
        equity_curve : pd.Series
            Account equity indexed by trade exit_time.
        daily_returns : pd.Series
            Daily percentage returns from the equity curve.
        start_of_day_to_trough_drawdown : list[dict]
            One record per trading day with sod_equity, trough_equity,
            drawdown, drawdown_pct, and any limit-hit flags.
        summary : dict
            Aggregate performance statistics including Topstep rule stats
            when enabled.
    """
    params = {**default_params(), **(params or {})}
    topstep = {**default_params()["topstep"], **(params.get("topstep") or {})}
    params["topstep"] = topstep

    if signals_df is None or signals_df.empty:
        empty_equity = pd.Series(dtype=float)
        empty_returns = pd.Series(dtype=float)
        return {
            "trades": pd.DataFrame(columns=REQUIRED_COLS + ["cost", "net_pnl", "trade_pnl_dollars", "equity", "skipped"]),
            "equity_curve": empty_equity,
            "daily_returns": empty_returns,
            "start_of_day_to_trough_drawdown": [],
            "summary": _empty_summary(params),
        }

    missing = [c for c in REQUIRED_COLS if c not in signals_df.columns]
    if missing:
        raise ValueError(f"signals_df missing required columns: {missing}")

    # Work on a copy and ensure chronological order.
    trades = signals_df.copy()
    trades["entry_time"] = pd.to_datetime(trades["entry_time"])
    trades["exit_time"] = pd.to_datetime(trades["exit_time"])
    trades = trades.sort_values("entry_time").reset_index(drop=True)

    cost_per_trade = float(params["slippage"]) + float(params["commission"])
    point_value = float(params["point_value"])
    initial_capital = float(params["initial_capital"])

    trades["cost"] = cost_per_trade
    trades["net_pnl"] = trades["pnl"] - cost_per_trade
    trades["trade_pnl_dollars"] = trades["net_pnl"] * point_value

    if topstep.get("enabled"):
        trades = _apply_topstep_rules(trades, topstep, initial_capital, point_value)
    else:
        trades["skipped"] = False
        trades["skip_reason"] = None
        trades["cumulative_pnl_dollars"] = trades["trade_pnl_dollars"].cumsum()
        trades["equity"] = initial_capital + trades["cumulative_pnl_dollars"]

    # Equity curve indexed by the exact exit timestamp of each executed trade.
    executed = trades[~trades["skipped"]]
    equity_curve = pd.Series(
        executed["equity"].to_numpy(),
        index=executed["exit_time"],
        name="equity",
    )

    # Daily returns: last observed equity each calendar day, forward-filled.
    daily_equity = equity_curve.resample("D").last().ffill()
    daily_returns = daily_equity.pct_change().dropna()

    sod_drawdowns = _start_of_day_drawdowns(executed, initial_capital)

    summary = _build_summary(
        trades=trades,
        executed=executed,
        equity_curve=equity_curve,
        daily_returns=daily_returns,
        sod_drawdowns=sod_drawdowns,
        initial_capital=initial_capital,
        topstep=topstep,
    )

    return {
        "trades": trades,
        "equity_curve": equity_curve,
        "daily_returns": daily_returns,
        "start_of_day_to_trough_drawdown": sod_drawdowns,
        "summary": summary,
    }


def _apply_topstep_rules(
    trades: pd.DataFrame,
    topstep: Dict[str, Any],
    initial_capital: float,
    point_value: float,
) -> pd.DataFrame:
    """Walk through trades and enforce Topstep daily/trailing drawdown limits.

    Trades are marked ``skipped=True`` when a rule prevents execution.
    Once the trailing limit is hit, all remaining trades are skipped unless
    ``reset_on_failure`` is enabled, in which case the account is reset to
    ``initial_capital`` and trading continues (modeling buying a new combine).
    Once the daily limit is hit, remaining trades on the same calendar day
    are skipped; trading resumes on the next session.
    Reaching the profit target is recorded as a flag but does NOT stop trading,
    because the goal is to evaluate ongoing strategy performance / payout
    potential, not just the combine pass/fail gate.
    """
    daily_limit = float(topstep["daily_drawdown_limit"])
    trailing_limit = float(topstep["trailing_drawdown_limit"])
    profit_target = float(topstep["profit_target"])
    reset_on_failure = bool(topstep.get("reset_on_failure", False))

    equity = initial_capital
    peak_equity = initial_capital
    daily_start_equity: float | None = None
    current_day: pd.Timestamp | None = None
    daily_limit_hit = False
    trailing_failed = False
    profit_target_reached = False
    skipped_flags = []
    skip_reasons = []
    equities = []
    account_ids = []
    cumulative_pnls = []
    cumulative_pnl = 0.0
    account_id = 0
    account_blowups = 0

    for _, row in trades.iterrows():
        exit_day = row["exit_time"].normalize()

        if current_day is None or exit_day != current_day:
            current_day = exit_day
            daily_start_equity = equity
            daily_limit_hit = False

        if trailing_failed and not reset_on_failure:
            skipped_flags.append(True)
            skip_reasons.append("trailing_limit")
            equities.append(equity)
            account_ids.append(account_id)
            cumulative_pnls.append(cumulative_pnl)
            continue

        if daily_limit_hit:
            skipped_flags.append(True)
            skip_reasons.append("daily_limit")
            equities.append(equity)
            account_ids.append(account_id)
            cumulative_pnls.append(cumulative_pnl)
            continue

        prospective_equity = equity + row["trade_pnl_dollars"]

        # Check daily drawdown limit (start-of-day to trough).
        daily_dd = daily_start_equity - prospective_equity
        if daily_dd > daily_limit:
            skipped_flags.append(True)
            skip_reasons.append("daily_limit")
            equities.append(equity)
            account_ids.append(account_id)
            cumulative_pnls.append(cumulative_pnl)
            daily_limit_hit = True
            continue

        # Check trailing drawdown limit (peak to trough).
        prospective_peak = max(peak_equity, prospective_equity)
        prospective_trough_dd = prospective_peak - prospective_equity
        if prospective_trough_dd > trailing_limit:
            if reset_on_failure:
                # Trade executes and blows the current account. Count the loss,
                # then reset capital and continue with the next trade.
                equity = prospective_equity
                peak_equity = prospective_peak
                cumulative_pnl += row["trade_pnl_dollars"]
                skipped_flags.append(False)
                skip_reasons.append(None)
                equities.append(equity)
                account_ids.append(account_id)
                cumulative_pnls.append(cumulative_pnl)
                account_blowups += 1
                account_id += 1
                equity = initial_capital
                peak_equity = initial_capital
                daily_start_equity = equity
                daily_limit_hit = False
                continue
            skipped_flags.append(True)
            skip_reasons.append("trailing_limit")
            equities.append(equity)
            account_ids.append(account_id)
            cumulative_pnls.append(cumulative_pnl)
            trailing_failed = True
            continue

        # Trade executes.
        equity = prospective_equity
        peak_equity = prospective_peak
        cumulative_pnl += row["trade_pnl_dollars"]
        if not profit_target_reached and equity >= initial_capital + profit_target:
            profit_target_reached = True
        skipped_flags.append(False)
        skip_reasons.append(None)
        equities.append(equity)
        account_ids.append(account_id)
        cumulative_pnls.append(cumulative_pnl)

    trades = trades.copy()
    trades["skipped"] = skipped_flags
    trades["skip_reason"] = skip_reasons
    trades["equity"] = equities
    trades["account_id"] = account_ids
    trades["cumulative_pnl_dollars"] = cumulative_pnls
    trades.attrs["account_blowups"] = account_blowups
    return trades


def _start_of_day_drawdowns(trades: pd.DataFrame, initial_capital: float) -> List[Dict[str, Any]]:
    """Compute start-of-day to intraday trough drawdown for each day."""
    if trades.empty:
        return []

    trades = trades.copy()
    trades["entry_date"] = trades["entry_time"].dt.normalize()
    trades["exit_date"] = trades["exit_time"].dt.normalize()

    records: List[Dict[str, Any]] = []
    prev_close = initial_capital

    for date, day in trades.groupby("entry_date"):
        start_equity = prev_close
        # Running equity within this day uses the previous close as the base.
        day_equities = start_equity + day["trade_pnl_dollars"].cumsum().to_numpy()
        trough = float(day_equities.min())
        dd = max(0.0, start_equity - trough)    # positive dollar drawdown
        dd_pct = -dd / start_equity if start_equity else np.nan  # negative pct
        records.append(
            {
                "date": pd.Timestamp(date),
                "sod_equity": start_equity,
                "trough_equity": trough,
                "drawdown": dd,
                "drawdown_pct": dd_pct,
            }
        )
        # Close for this day = equity after the last trade of the day.
        prev_close = float(day_equities[-1])

    return records


def _build_summary(
    trades: pd.DataFrame,
    executed: pd.DataFrame,
    equity_curve: pd.Series,
    daily_returns: pd.Series,
    sod_drawdowns: List[Dict[str, Any]],
    initial_capital: float,
    topstep: Dict[str, Any],
) -> Dict[str, Any]:
    """Assemble aggregate backtest statistics."""
    net_pnls = executed["net_pnl"].to_numpy() if not executed.empty else np.array([])
    wins = net_pnls[net_pnls > 0]
    losses = net_pnls[net_pnls < 0]

    gross_profit = float(wins.sum()) if len(wins) else 0.0
    gross_loss = float(losses.sum()) if len(losses) else 0.0
    profit_factor = (
        gross_profit / abs(gross_loss) if gross_loss != 0 else np.inf
    )

    # Peak-to-trough max drawdown on the trade-by-trade equity curve.
    if not equity_curve.empty:
        equity_values = equity_curve.to_numpy()
        running_max = np.maximum.accumulate(equity_values)
        drawdowns = (equity_values - running_max) / running_max
        max_dd_pct = float(drawdowns.min())
        max_dd_dollar = float((equity_values - running_max).min())
    else:
        max_dd_pct = 0.0
        max_dd_dollar = 0.0

    # Start-of-day drawdowns.
    if sod_drawdowns:
        dds = [d["drawdown"] for d in sod_drawdowns]
        dd_pcts = [d["drawdown_pct"] for d in sod_drawdowns]
        max_sod_dd_dollar = float(max(dds))
        max_sod_dd_pct = float(min(dd_pcts))
        avg_sod_dd_dollar = float(np.mean(dds))
        avg_sod_dd_pct = float(np.mean(dd_pcts))
    else:
        max_sod_dd_dollar = 0.0
        max_sod_dd_pct = 0.0
        avg_sod_dd_dollar = 0.0
        avg_sod_dd_pct = 0.0

    final_equity = float(equity_curve.iloc[-1]) if not equity_curve.empty else initial_capital
    total_return_pct = (final_equity - initial_capital) / initial_capital

    # When resetting blown accounts, the meaningful "total return" is the
    # cumulative realized PnL across all accounts, not the equity of the last
    # active account.  total_pnl_dollars is kept for backward compatibility.
    cumulative_pnl_dollars = (
        float(trades["cumulative_pnl_dollars"].iloc[-1])
        if not trades.empty and "cumulative_pnl_dollars" in trades.columns
        else final_equity - initial_capital
    )

    # Hold-duration statistics for executed trades (exit_time - entry_time).
    if not executed.empty:
        hold_deltas = executed["exit_time"] - executed["entry_time"]
        hold_seconds = hold_deltas.dt.total_seconds().to_numpy()
        avg_hold = float(np.mean(hold_seconds))
        median_hold = float(np.median(hold_seconds))
        p10_hold = float(np.percentile(hold_seconds, 10))
        p90_hold = float(np.percentile(hold_seconds, 90))
    else:
        avg_hold = 0.0
        median_hold = 0.0
        p10_hold = 0.0
        p90_hold = 0.0

    summary: Dict[str, Any] = {
        "initial_capital": initial_capital,
        "final_equity": final_equity,
        "total_return_pct": total_return_pct,
        "total_trades": int(len(trades)),
        "executed_trades": int(len(executed)),
        "skipped_trades": int(len(trades) - len(executed)),
        "winning_trades": int(len(wins)),
        "losing_trades": int(len(losses)),
        "win_rate": float(len(wins) / len(executed)) if len(executed) else 0.0,
        "total_pnl_points": float(trades["pnl"].sum()),
        "net_pnl_points": float(net_pnls.sum()),
        "gross_profit_points": gross_profit,
        "gross_loss_points": gross_loss,
        "profit_factor": profit_factor,
        "avg_pnl_points": float(net_pnls.mean()) if len(net_pnls) else 0.0,
        "avg_win_points": float(wins.mean()) if len(wins) else 0.0,
        "avg_loss_points": float(losses.mean()) if len(losses) else 0.0,
        "max_drawdown_pct": max_dd_pct,
        "max_drawdown_dollar": max_dd_dollar,
        "max_sod_drawdown_dollar": max_sod_dd_dollar,
        "max_sod_drawdown_pct": max_sod_dd_pct,
        "avg_sod_drawdown_dollar": avg_sod_dd_dollar,
        "avg_sod_drawdown_pct": avg_sod_dd_pct,
        "avg_daily_return": float(daily_returns.mean()) if len(daily_returns) else 0.0,
        "daily_volatility": float(daily_returns.std()) if len(daily_returns) else 0.0,
        "cumulative_pnl_dollars": cumulative_pnl_dollars,
        "avg_hold_seconds": avg_hold,
        "median_hold_seconds": median_hold,
        "p10_hold_seconds": p10_hold,
        "p90_hold_seconds": p90_hold,
    }

    # Topstep-specific stats.
    summary["topstep_enabled"] = bool(topstep.get("enabled"))
    if topstep.get("enabled"):
        skip_reasons = trades["skip_reason"].fillna("")
        summary["daily_limit_hits"] = int((skip_reasons == "daily_limit").sum())
        summary["trailing_limit_hits"] = int((skip_reasons == "trailing_limit").sum())
        summary["account_failed"] = bool((skip_reasons == "trailing_limit").any())
        summary["account_blowups"] = int(trades.attrs.get("account_blowups", 0))
        summary["profit_target_reached"] = bool(
            final_equity >= initial_capital + float(topstep["profit_target"])
            or cumulative_pnl_dollars >= float(topstep["profit_target"])
        )
        summary["profit_target"] = float(topstep["profit_target"])
        summary["daily_drawdown_limit"] = float(topstep["daily_drawdown_limit"])
        summary["trailing_drawdown_limit"] = float(topstep["trailing_drawdown_limit"])
        summary["reset_on_failure"] = bool(topstep.get("reset_on_failure", False))
    else:
        summary["daily_limit_hits"] = 0
        summary["trailing_limit_hits"] = 0
        summary["account_failed"] = False
        summary["account_blowups"] = 0
        summary["profit_target_reached"] = False
        summary["profit_target"] = float(topstep["profit_target"])
        summary["daily_drawdown_limit"] = float(topstep["daily_drawdown_limit"])
        summary["trailing_drawdown_limit"] = float(topstep["trailing_drawdown_limit"])
        summary["reset_on_failure"] = False

    return summary


def _empty_summary(params: Dict[str, Any]) -> Dict[str, Any]:
    """Summary when no trades are provided."""
    initial = float(params["initial_capital"])
    topstep = {**default_params()["topstep"], **(params.get("topstep") or {})}
    return {
        "initial_capital": initial,
        "final_equity": initial,
        "total_return_pct": 0.0,
        "total_trades": 0,
        "executed_trades": 0,
        "skipped_trades": 0,
        "winning_trades": 0,
        "losing_trades": 0,
        "win_rate": 0.0,
        "total_pnl_points": 0.0,
        "net_pnl_points": 0.0,
        "gross_profit_points": 0.0,
        "gross_loss_points": 0.0,
        "profit_factor": np.inf,
        "avg_pnl_points": 0.0,
        "avg_win_points": 0.0,
        "avg_loss_points": 0.0,
        "max_drawdown_pct": 0.0,
        "max_drawdown_dollar": 0.0,
        "max_sod_drawdown_dollar": 0.0,
        "max_sod_drawdown_pct": 0.0,
        "avg_sod_drawdown_dollar": 0.0,
        "avg_sod_drawdown_pct": 0.0,
        "avg_daily_return": 0.0,
        "daily_volatility": 0.0,
        "avg_hold_seconds": 0.0,
        "median_hold_seconds": 0.0,
        "p10_hold_seconds": 0.0,
        "p90_hold_seconds": 0.0,
        "topstep_enabled": bool(topstep.get("enabled")),
        "daily_limit_hits": 0,
        "trailing_limit_hits": 0,
        "account_failed": False,
        "profit_target_reached": False,
        "profit_target": float(topstep["profit_target"]),
        "daily_drawdown_limit": float(topstep["daily_drawdown_limit"]),
        "trailing_drawdown_limit": float(topstep["trailing_drawdown_limit"]),
    }
