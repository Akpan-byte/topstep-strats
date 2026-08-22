# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created engine_rust/scripts/portfolio_runner.py.
#   - Runs a single Paper-1 strategy across NQ/ES/YM x Asian/London/NY (9 legs).
#   - Converts per-leg point PnL to dollars, aggregates by calendar day, and
#     applies Topstep rules (daily $900, trailing $2,000, profit target $3,000)
#     at the portfolio/account level with reset-on-failure semantics.
#   - Computes portfolio-level win rate, total PnL, avg/week, max drawdown,
#     Sharpe, and account blowup/resets.
# 2026-08-20  kilo
#   - Fixed daily-limit handling in _apply_portfolio_topstep to skip the day's
#     PnL without resetting to starting cash, matching the Rust engine's
#     per-trade daily-limit behavior.  Trailing-limit breaches still realize the
#     loss and reset when reset_on_failure is enabled.
# WHY: The previous daily-aggregation logic reset capital on daily breaches,
#      which overstated blowups and diverged from the Rust engine semantics.

#!/usr/bin/env python3
"""Portfolio-level Paper-1 runner (NQ/ES/YM x Asian/London/NY)."""
from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

import numpy as np
import pandas as pd

# Path setup so this module can be imported from sweep_engine.py or run standalone.
_ENGINE_RUST_ROOT = Path(__file__).resolve().parent.parent
_PROJECT_ROOT = _ENGINE_RUST_ROOT.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))
if str(_ENGINE_RUST_ROOT / "python") not in sys.path:
    sys.path.insert(0, str(_ENGINE_RUST_ROOT / "python"))

from scripts.runner_exit_numba import apply_runner_to_signals
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import (
    generate_signals as generate_paper1_signals,
    get_strategy_config as get_paper1_config,
    list_strategy_ids as list_paper1_ids,
    _atr as _paper1_atr,
)


POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}
TICK_SIZES = {"NQ": 0.25, "ES": 0.25, "YM": 1.0}

PAPER1_SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

INSTRUMENTS = ["NQ", "ES", "YM"]
SESSIONS = ["Asian", "London", "NY"]


@dataclass
class PortfolioResult:
    """Summary statistics for a 9-leg portfolio run."""

    strategy_id: str
    tp: float
    sl: float
    mode: str
    n_legs: int
    n_trades: int
    win_rate: float
    total_pnl_dollars: float
    avg_per_week: float
    max_drawdown: float
    sharpe: float
    account_blowups: int
    daily_limit_hits: int
    trailing_limit_hits: int
    profit_target_reached: bool
    account_failed: bool


def _build_paper1_entry_cfg(
    sid: str, instrument: str, session: str, start: str, end: str
) -> Dict[str, Any]:
    """Build a Paper-1 config for signal generation only (exits are re-simulated)."""
    cfg = get_paper1_config(sid)
    cfg.update(
        {
            "instrument": instrument,
            "tick_size": TICK_SIZES[instrument],
            "point_value": POINT_VALUES[instrument],
            "session": session,
            "session_start": PAPER1_SESSIONS[session][0],
            "session_end": PAPER1_SESSIONS[session][1],
            "tp_atr": 2.0,
            "sl_atr": 5.0,
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def _attach_tp_sl(
    entry_signals: pd.DataFrame, tp: float, sl: float, tick_size: float
) -> pd.DataFrame:
    """Return a copy of entry signals with stop_loss/take_profit columns."""
    if entry_signals.empty:
        return entry_signals.copy()
    signals = entry_signals.copy()
    atr = signals["atr_value"].values
    direction = signals["direction"].values
    entry_price = signals["entry_price"].values

    stop_loss = np.where(
        direction == 1,
        entry_price - sl * atr - tick_size,
        entry_price + sl * atr + tick_size,
    )
    take_profit = np.where(
        direction == 1,
        entry_price + tp * atr + tick_size,
        entry_price - tp * atr - tick_size,
    )

    signals["stop_loss"] = stop_loss
    signals["take_profit"] = take_profit
    return signals


def _runner_kwargs(
    mode_name: str, base_kwargs: Dict[str, Any], session_end: str
) -> Dict[str, Any]:
    """Build kwargs for apply_runner_to_signals."""
    kw = dict(base_kwargs)
    kw["session_end_time"] = pd.Timestamp(session_end).time()
    if mode_name == "hold_day":
        kw["day_end_time"] = pd.Timestamp("16:00").time()
    return kw


def _load_instrument_df(instrument: str, data_dir: str) -> pd.DataFrame:
    """Load an instrument's 1-minute data (cached on the DataFrame)."""
    path = Path(data_dir) / f"{instrument}_1min.parquet"
    if not path.exists():
        csv = Path(data_dir) / f"{instrument}_1min.csv"
        if csv.exists():
            path = csv
    df = load_market_data(str(path))
    df = df.copy()
    df["atr"] = _paper1_atr(df, 14)
    return df


def _generate_leg_trades(
    sid: str,
    instrument: str,
    session: str,
    tp: float,
    sl: float,
    mode_name: str,
    base_kwargs: Dict[str, Any],
    start: str,
    end: str,
    data_dir: str,
) -> Optional[pd.DataFrame]:
    """Generate and exit-simulate trades for one (strategy, instrument, session) leg."""
    df = _load_instrument_df(instrument, data_dir)
    cfg = _build_paper1_entry_cfg(sid, instrument, session, start, end)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start, end)

    try:
        entry_signals = generate_paper1_signals(df_s, cfg, simulate_exits=False)
    except Exception as exc:  # pragma: no cover - defensive
        print(f"  signal generation failed: {sid} {instrument} {session} {exc}", file=sys.stderr)
        return None

    if entry_signals.empty:
        return None

    df_full = split_by_date(df.copy(), start, end)
    signals_with_sl_tp = _attach_tp_sl(entry_signals, tp, sl, TICK_SIZES[instrument])
    kw = _runner_kwargs(mode_name, base_kwargs, cfg["session_end"])
    trades = apply_runner_to_signals(df_full, signals_with_sl_tp, **kw)
    if trades.empty:
        return None

    # Convert point PnL to dollar PnL and tag leg metadata.
    trades["dollar_pnl"] = trades["pnl"] * POINT_VALUES[instrument]
    trades["instrument"] = instrument
    trades["session"] = session
    trades["point_value"] = POINT_VALUES[instrument]
    return trades


def _aggregate_daily_pnl(trades: pd.DataFrame) -> pd.Series:
    """Sum dollar PnL per calendar day across all legs."""
    idx = pd.to_datetime(trades["exit_time"])
    if idx.dt.tz is None:
        idx = idx.dt.tz_localize("UTC")
    else:
        idx = idx.dt.tz_convert("UTC")
    trades = trades.copy()
    trades["exit_day"] = idx.dt.floor("D")
    return trades.groupby("exit_day")["dollar_pnl"].sum().sort_index()


def _apply_portfolio_topstep(
    daily_pnl: pd.Series,
    starting_cash: float = 100_000.0,
    daily_drawdown_limit: float = 900.0,
    trailing_drawdown_limit: float = 2_000.0,
    profit_target: float = 3_000.0,
    reset_on_failure: bool = True,
) -> Tuple[pd.Series, Dict[str, Any]]:
    """Apply account-level Topstep rules to a daily PnL series.

    Returns the post-rule equity curve and a stat dictionary.  When a rule is
    breached the account is reset to starting cash (matching the Rust engine's
    reset_on_failure semantics).  The daily PnL that caused the breach is
    realized for trailing-drawdown breaches and excluded for daily-limit
    breaches, which is a conservative daily-aggregation approximation of the
    per-trade Rust logic.
    """
    equity = starting_cash
    high_water = starting_cash
    daily_limit_hits = 0
    trailing_limit_hits = 0
    blowups = 0
    account_failed = False
    profit_target_reached = False
    equity_curve = [equity]
    realized_pnl = []

    for day, pnl in daily_pnl.items():
        if account_failed:
            break

        daily_start = equity
        prospective_equity = equity + pnl
        prospective_high_water = max(high_water, prospective_equity)

        daily_breached = prospective_equity < daily_start - daily_drawdown_limit
        trailing_breached = prospective_equity < prospective_high_water - trailing_drawdown_limit

        if daily_breached:
            daily_limit_hits += 1
            # Skip the day's PnL; equity stays at daily_start.  This matches the
            # Rust engine per-trade behavior where daily-limit trades are not
            # executed and the rest of the calendar day is skipped.
            realized_pnl.append(0.0)
            equity_curve.append(equity)
            continue

        if trailing_breached:
            trailing_limit_hits += 1
            blowups += 1
            # Realize the PnL that caused the breach, then reset if configured.
            equity = prospective_equity
            high_water = prospective_high_water
            realized_pnl.append(pnl)
            equity_curve.append(equity)
            if reset_on_failure:
                equity = starting_cash
                high_water = starting_cash
            else:
                account_failed = True
            continue

        # Day executes normally.
        equity = prospective_equity
        high_water = prospective_high_water
        realized_pnl.append(pnl)
        equity_curve.append(equity)

        if not profit_target_reached and equity >= starting_cash + profit_target:
            profit_target_reached = True

    stats = {
        "daily_limit_hits": daily_limit_hits,
        "trailing_limit_hits": trailing_limit_hits,
        "account_blowups": blowups,
        "account_failed": account_failed,
        "profit_target_reached": profit_target_reached,
        "realized_pnl": np.array(realized_pnl, dtype=float),
        "equity_curve": np.array(equity_curve, dtype=float),
    }
    return pd.Series(realized_pnl, index=daily_pnl.index[: len(realized_pnl)]), stats


def _compute_metrics(
    all_trades: pd.DataFrame,
    realized_daily: pd.Series,
    topstep_stats: Dict[str, Any],
    start_date: str,
    end_date: str,
) -> Dict[str, Any]:
    """Compute portfolio-level performance metrics."""
    dollar_pnls = all_trades["dollar_pnl"].values
    n_trades = len(dollar_pnls)
    win_rate = float(np.mean(dollar_pnls > 0)) if n_trades else 0.0
    total_pnl = float(dollar_pnls.sum()) if n_trades else 0.0

    equity_curve = topstep_stats["equity_curve"]
    max_dd = 0.0
    peak = equity_curve[0]
    for val in equity_curve:
        if val > peak:
            peak = val
        dd = peak - val
        if dd > max_dd:
            max_dd = dd

    daily_returns = topstep_stats["realized_pnl"] / 100_000.0
    sharpe = 0.0
    if len(daily_returns) > 1 and daily_returns.std() > 0:
        sharpe = float(daily_returns.mean() / daily_returns.std() * np.sqrt(252))

    start = pd.Timestamp(start_date)
    end = pd.Timestamp(end_date)
    weeks = max(1.0, (end - start).days / 7.0)
    avg_per_week = total_pnl / weeks

    return {
        "n_trades": n_trades,
        "win_rate": win_rate,
        "total_pnl_dollars": total_pnl,
        "avg_per_week": avg_per_week,
        "max_drawdown": max_dd,
        "sharpe": sharpe,
        "account_blowups": topstep_stats["account_blowups"],
        "daily_limit_hits": topstep_stats["daily_limit_hits"],
        "trailing_limit_hits": topstep_stats["trailing_limit_hits"],
        "profit_target_reached": topstep_stats["profit_target_reached"],
        "account_failed": topstep_stats["account_failed"],
    }


def run_portfolio(
    sid: str,
    tp: float,
    sl: float,
    mode_name: str,
    base_kwargs: Dict[str, Any],
    start_date: str,
    end_date: str,
    data_dir: str,
    topstep_enabled: bool = True,
) -> PortfolioResult:
    """Run a Paper-1 strategy as a 9-leg portfolio and return summary stats."""
    legs: List[pd.DataFrame] = []
    for instrument in INSTRUMENTS:
        for session in SESSIONS:
            trades = _generate_leg_trades(
                sid, instrument, session, tp, sl, mode_name, base_kwargs,
                start_date, end_date, data_dir,
            )
            if trades is not None and not trades.empty:
                legs.append(trades)

    if not legs:
        return PortfolioResult(
            strategy_id=sid,
            tp=tp,
            sl=sl,
            mode=mode_name,
            n_legs=0,
            n_trades=0,
            win_rate=0.0,
            total_pnl_dollars=0.0,
            avg_per_week=0.0,
            max_drawdown=0.0,
            sharpe=0.0,
            account_blowups=0,
            daily_limit_hits=0,
            trailing_limit_hits=0,
            profit_target_reached=False,
            account_failed=False,
        )

    all_trades = pd.concat(legs, ignore_index=True)
    daily_pnl = _aggregate_daily_pnl(all_trades)

    if topstep_enabled:
        realized_daily, topstep_stats = _apply_portfolio_topstep(daily_pnl)
    else:
        realized_daily = daily_pnl
        topstep_stats = {
            "account_blowups": 0,
            "daily_limit_hits": 0,
            "trailing_limit_hits": 0,
            "profit_target_reached": False,
            "account_failed": False,
            "realized_pnl": daily_pnl.values,
            "equity_curve": np.cumsum(np.concatenate([[0.0], daily_pnl.values])) + 100_000.0,
        }

    metrics = _compute_metrics(
        all_trades, realized_daily, topstep_stats, start_date, end_date
    )

    return PortfolioResult(
        strategy_id=sid,
        tp=tp,
        sl=sl,
        mode=mode_name,
        n_legs=len(legs),
        **metrics,
    )


def portfolio_result_to_record(result: PortfolioResult) -> Dict[str, Any]:
    """Convert a PortfolioResult to a flat dictionary for CSV output."""
    return {
        "paper": "portfolio1",
        "strategy_id": result.strategy_id,
        "tp": result.tp,
        "sl": result.sl,
        "mode": result.mode,
        "n_legs": result.n_legs,
        "n_trades": result.n_trades,
        "win_rate": result.win_rate,
        "total_pnl_dollars": result.total_pnl_dollars,
        "avg_per_week": result.avg_per_week,
        "max_drawdown": result.max_drawdown,
        "sharpe": result.sharpe,
        "account_blowups": result.account_blowups,
        "daily_limit_hits": result.daily_limit_hits,
        "trailing_limit_hits": result.trailing_limit_hits,
        "profit_target_reached": result.profit_target_reached,
        "account_failed": result.account_failed,
    }


OUTPUT_COLUMNS_PORTFOLIO1: Sequence[str] = [
    "paper",
    "strategy_id",
    "tp",
    "sl",
    "mode",
    "n_legs",
    "n_trades",
    "win_rate",
    "total_pnl_dollars",
    "avg_per_week",
    "max_drawdown",
    "sharpe",
    "account_blowups",
    "daily_limit_hits",
    "trailing_limit_hits",
    "profit_target_reached",
    "account_failed",
]


def _portfolio_tasks(args: Any) -> List[Tuple[str, float, float, str, Dict[str, Any], str, str, str, bool]]:
    """Build portfolio1 task tuples from CLI arguments.

    Each task runs one Paper-1 strategy ID as a 9-leg portfolio.  If no
    mode/TP/SL is supplied a sensible default combo is used so the runner is
    usable out of the box.
    """
    sids = list_paper1_ids()[args.id_start : args.id_end]
    start = args.start_date
    end = args.end_date
    # Portfolio1 is explicitly an account-level Topstep analysis; default on.
    topstep = getattr(args, "topstep", True) or True

    if args.mode is None:
        # Default out-of-the-box combo.
        combos = [
            ("breakeven_0.5x", {"mode": "breakeven_then_trail", "trail_atr_mult": 0.5}, 0.5, 6.0),
        ]
    else:
        mode = args.mode
        trail_mult = getattr(args, "trail_atr_mult", None)
        if trail_mult is None and mode in ("trail", "breakeven_then_trail"):
            trail_mult = 0.5
        base_kwargs: Dict[str, Any] = {"mode": mode}
        if trail_mult is not None:
            base_kwargs["trail_atr_mult"] = trail_mult
        mode_name = f"{mode}_{trail_mult}x" if trail_mult is not None else mode
        tp = args.tp_atr if args.tp_atr is not None else 0.5
        sl = args.sl_atr if args.sl_atr is not None else 6.0
        combos = [(mode_name, base_kwargs, tp, sl)]

    return [
        (sid, tp, sl, mode_name, base_kwargs, start, end, args.data_dir, topstep)
        for sid in sids
        for mode_name, base_kwargs, tp, sl in combos
    ]


def _run_portfolio_group(
    args: Tuple[str, float, float, str, Dict[str, Any], str, str, str, bool],
) -> List[Dict[str, Any]]:
    """Worker entry point for one portfolio task."""
    sid, tp, sl, mode_name, base_kwargs, start, end, data_dir, topstep_enabled = args
    result = run_portfolio(
        sid=sid,
        tp=tp,
        sl=sl,
        mode_name=mode_name,
        base_kwargs=base_kwargs,
        start_date=start,
        end_date=end,
        data_dir=data_dir,
        topstep_enabled=topstep_enabled,
    )
    return [portfolio_result_to_record(result)]
