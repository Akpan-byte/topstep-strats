#!/usr/bin/env python3
# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created /home/akpan/topstep-strats/stack_simulator.py.
#   - Generates Paper-1 entry signals for a small NQ portfolio, simulates exits
#     via scripts.runner_exit_numba, and applies a 2-contract global position cap.
#   - Tests Rule A (top 2 active signals, 1 contract each), Rule B (best only, 2
#     contracts), Rule C (conviction-based best only, 2 contracts), and a single
#     best-leg baseline at 2 contracts.
#   - Runs combined trade streams through agnostic_backtest with Topstep rules
#     ($900 daily DD, $2,000 trailing DD, $3,000 profit target, reset on failure).
#   - Added --payout-mode: uses topstep_payout.simulate_topstep_payouts to model
#     50k/150k Standard and Consistency accounts with 40% consistency rule,
#     winning-day minimums, payout caps, and account resets on trailing failure.
#   - Writes daily equity CSV to gh_results/rust_sweep_v3/stack_sim_results.csv.
# WHY: Answer whether the $5k-$10k/week target is achievable under a realistic
#      Topstep combine cap and report net cash extracted, not just equity growth.

"""Realistic execution simulator for a 2-contract NQ stack under Topstep rules.

Builds a small portfolio of non-correlated Paper-1 legs, applies a global
2-contract position cap with several allocation rules, runs the combined
trade stream through the Rust engine for PnL accounting, and reports
Topstep-aware performance (resets, drawdowns, weekly averages).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any, Dict, List, Sequence, Tuple

import numpy as np
import pandas as pd

# Rust engine
from agnostic_backtest import AgnosticBacktest, TopstepConfig

# Payout simulator
from topstep_payout import TOPSTEP_SPECS, simulate_topstep_payouts

# Project helpers
from scripts.runner_exit_numba import apply_runner_to_signals
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.strategies.paper1_matrix import (
    generate_signals as generate_paper1_signals,
    get_strategy_config as get_paper1_config,
    _atr as paper1_atr,
)

# ---------------------------------------------------------------------------
# Constants matching the sweep_engine.py convention
# ---------------------------------------------------------------------------
PROJECT_ROOT = Path(__file__).resolve().parent
DATA_DIR = PROJECT_ROOT / "data"
SWEEP_CSV = (
    PROJECT_ROOT
    / "gh_results"
    / "rust_sweep_v3"
    / "paper1_rust_sweep.csv"
)
OUTPUT_CSV = (
    PROJECT_ROOT
    / "gh_results"
    / "rust_sweep_v3"
    / "stack_sim_results.csv"
)

PAPER1_SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

POINT_VALUES = {"NQ": 20.0, "ES": 50.0, "YM": 5.0}
TICK_SIZES = {"NQ": 0.25, "ES": 0.25, "YM": 1.0}

TOPSTEP_DAILY_DD = 900.0
TOPSTEP_TRAILING_DD = 2_000.0
TOPSTEP_PROFIT_TARGET = 3_000.0
INITIAL_CAPITAL = 100_000.0

# ---------------------------------------------------------------------------
# Default portfolio (user-supplied starting set)
# ---------------------------------------------------------------------------
DEFAULT_PORTFOLIO: List[Dict[str, Any]] = [
    {"strategy_id": "51", "instrument": "NQ", "session": "London", "tp": 0.3, "sl": 10.0, "mode": "hold_day"},
    {"strategy_id": "03", "instrument": "NQ", "session": "London", "tp": 0.3, "sl": 8.0, "mode": "hold_day"},
    {"strategy_id": "09", "instrument": "NQ", "session": "NY",     "tp": 2.0, "sl": 10.0, "mode": "trail_0.5x"},
    {"strategy_id": "13", "instrument": "NQ", "session": "NY",     "tp": 2.0, "sl": 10.0, "mode": "breakeven_0.5x"},
    {"strategy_id": "58", "instrument": "NQ", "session": "London", "tp": 2.0, "sl": 10.0, "mode": "trail_1.0x"},
    {"strategy_id": "61", "instrument": "NQ", "session": "NY",     "tp": 2.0, "sl": 6.0,  "mode": "breakeven_0.5x"},
]


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------
_data_cache: Dict[str, pd.DataFrame] = {}


def load_instrument_df(instrument: str, data_dir: Path = DATA_DIR) -> pd.DataFrame:
    """Load a 1-minute parquet and pre-compute ATR for the runner."""
    if instrument in _data_cache:
        return _data_cache[instrument].copy()

    path = data_dir / f"{instrument}_1min.parquet"
    if not path.exists():
        path = data_dir / f"{instrument}_1min.csv"
    df = load_market_data(str(path)).copy()
    df["atr"] = paper1_atr(df, 14)
    _data_cache[instrument] = df
    return df.copy()


# ---------------------------------------------------------------------------
# Signal generation helpers
# ---------------------------------------------------------------------------
def build_entry_cfg(
    strategy_id: Any,
    instrument: str,
    session: str,
    tp: float,
    sl: float,
    mode: str,
) -> Dict[str, Any]:
    """Build a Paper-1 parameter dict matching one sweep row."""
    cfg = get_paper1_config(f"{int(strategy_id):03d}")
    start, end = PAPER1_SESSIONS[session]
    cfg.update(
        {
            "instrument": instrument,
            "tick_size": TICK_SIZES[instrument],
            "point_value": POINT_VALUES[instrument],
            "session": session,
            "session_start": start,
            "session_end": end,
            "tp_atr": tp,
            "sl_atr": sl,
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
        }
    )
    return cfg


def attach_tp_sl(entry_signals: pd.DataFrame, tp: float, sl: float, tick_size: float) -> pd.DataFrame:
    """Attach stop-loss / take-profit columns using each signal's ATR."""
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


def runner_kwargs_for_mode(mode: str, session_end: str) -> Dict[str, Any]:
    """Map a sweep mode name to kwargs for apply_runner_to_signals."""
    mode_map: Dict[str, Dict[str, Any]] = {
        "baseline": {"mode": "baseline"},
        "trail_0.5x": {"mode": "trail", "trail_atr_mult": 0.5},
        "trail_1.0x": {"mode": "trail", "trail_atr_mult": 1.0},
        "breakeven_0.5x": {"mode": "breakeven_then_trail", "trail_atr_mult": 0.5},
        "breakeven_1.0x": {"mode": "breakeven_then_trail", "trail_atr_mult": 1.0},
        "hold_session": {"mode": "hold_session"},
        "hold_day": {"mode": "hold_day"},
    }
    if mode not in mode_map:
        raise ValueError(f"Unknown mode: {mode}")
    kw = dict(mode_map[mode])
    kw["session_end_time"] = pd.Timestamp(session_end).time()
    if mode == "hold_day":
        kw["day_end_time"] = pd.Timestamp("16:00").time()
    return kw


def generate_leg_trades(
    leg: Dict[str, Any],
    start_date: str = "2016-06-01",
    end_date: str = "2026-05-29",
) -> pd.DataFrame:
    """Generate finalized trades (entry + exit) for one portfolio leg."""
    instrument = leg["instrument"]
    session = leg["session"]
    cfg = build_entry_cfg(
        leg["strategy_id"], instrument, session, leg["tp"], leg["sl"], leg["mode"]
    )
    session_end = cfg["session_end"]

    df = load_instrument_df(instrument)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_session = split_by_date(df.loc[mask].copy(), start_date, end_date)
    df_full = split_by_date(df.copy(), start_date, end_date)

    entry_signals = generate_paper1_signals(df_session, cfg, simulate_exits=False)
    if entry_signals.empty:
        return pd.DataFrame(
            columns=[
                "entry_time", "direction", "entry_price", "stop_loss",
                "take_profit", "exit_time", "exit_price", "pnl", "exit_reason",
                "instrument", "leg",
            ]
        )

    signals_with_sl_tp = attach_tp_sl(
        entry_signals, leg["tp"], leg["sl"], TICK_SIZES[instrument]
    )
    kw = runner_kwargs_for_mode(leg["mode"], session_end)
    trades = apply_runner_to_signals(df_full, signals_with_sl_tp, **kw)
    trades["instrument"] = instrument
    trades["leg"] = f"{leg['strategy_id']}_{instrument}_{session}_{leg['mode']}"
    return trades


# ---------------------------------------------------------------------------
# Stack allocation rules under a 2-contract global cap
# ---------------------------------------------------------------------------
def apply_stack_rule(
    leg_trades: Sequence[pd.DataFrame],
    leg_meta: Sequence[Dict[str, Any]],
    rule: str,
    max_contracts: int = 2,
) -> pd.DataFrame:
    """Combine leg trades using the specified allocation rule.

    Parameters
    ----------
    leg_trades : sequence of DataFrames
        Finalized trades for each leg (entry_time, exit_time, pnl, ...).
    leg_meta : sequence of dicts
        Metadata for each leg; must contain 'priority' (for A/B) and
        'conviction' (for C).
    rule : {'A', 'B', 'C'}
        A = top 2 active signals get 1 contract each (single signal -> 2).
        B = only best active signal gets 2 contracts.
        C = dynamic conviction: best active signal by conviction gets 2.
    max_contracts : int
        Global position cap.

    Returns
    -------
    pd.DataFrame
        Combined trade stream with rows duplicated to represent contracts.
    """
    # Tag and concatenate
    tagged = []
    for i, trades in enumerate(leg_trades):
        if trades.empty:
            continue
        t = trades.copy()
        t["leg_id"] = i
        t["priority"] = leg_meta[i]["priority"]
        t["conviction"] = leg_meta[i]["conviction"]
        tagged.append(t)

    if not tagged:
        return pd.DataFrame(
            columns=[
                "entry_time", "direction", "entry_price", "stop_loss",
                "take_profit", "exit_time", "exit_price", "pnl", "exit_reason",
                "instrument", "leg", "leg_id",
            ]
        )

    candidates = pd.concat(tagged, ignore_index=True)
    candidates["entry_time"] = pd.to_datetime(candidates["entry_time"], utc=True)
    candidates["exit_time"] = pd.to_datetime(candidates["exit_time"], utc=True)
    candidates["entry_minute"] = candidates["entry_time"].dt.floor("min")
    candidates = candidates.sort_values(["entry_minute", "priority"], ascending=[True, False])

    expanded_rows: List[Dict[str, Any]] = []
    open_positions: List[Tuple[pd.Timestamp, int]] = []

    for minute, group in candidates.groupby("entry_minute", sort=True):
        # Drop positions that are closed by this minute
        open_positions = [(et, c) for et, c in open_positions if et > minute]
        open_contracts = sum(c for _, c in open_positions)
        available = max_contracts - open_contracts
        if available <= 0:
            continue

        if rule in ("A", "B"):
            group = group.sort_values("priority", ascending=False)
        else:  # rule C
            group = group.sort_values("conviction", ascending=False)

        allocations: List[Tuple[pd.Series, int]] = []
        if rule == "A":
            if len(group) == 1:
                allocations.append((group.iloc[0], min(2, available)))
            else:
                n_take = min(2, len(group), available)
                for i in range(n_take):
                    allocations.append((group.iloc[i], 1))
        elif rule == "B":
            allocations.append((group.iloc[0], min(2, available)))
        elif rule == "C":
            allocations.append((group.iloc[0], min(2, available)))
        else:
            raise ValueError(f"Unknown rule: {rule}")

        for row, contracts in allocations:
            if contracts <= 0:
                continue
            for _ in range(contracts):
                expanded_rows.append(row.to_dict())
            open_positions.append((row["exit_time"], contracts))

    if not expanded_rows:
        return pd.DataFrame(
            columns=[
                "entry_time", "direction", "entry_price", "stop_loss",
                "take_profit", "exit_time", "exit_price", "pnl", "exit_reason",
                "instrument", "leg", "leg_id",
            ]
        )

    return pd.DataFrame(expanded_rows).reset_index(drop=True)


# ---------------------------------------------------------------------------
# Topstep-aware daily equity
# ---------------------------------------------------------------------------
def simulate_topstep_equity(
    trades: pd.DataFrame,
    point_value: float,
    initial_capital: float = INITIAL_CAPITAL,
) -> pd.DataFrame:
    """Replicate the Rust engine's Topstep walk to build a daily equity curve.

    The Rust engine sorts trades by entry_ts, blocks the rest of any day that
    breaches the daily drawdown, and resets capital on trailing-drawdown
    failures when reset_on_failure=True.  We mirror that logic so the
    resulting equity curve matches the engine's summary stats.
    """
    if trades.empty:
        return pd.DataFrame(
            columns=["date", "equity"]
        ).set_index("date")

    df = trades.copy()
    df["entry_time"] = pd.to_datetime(df["entry_time"], utc=True)
    df["exit_time"] = pd.to_datetime(df["exit_time"], utc=True)
    df = df.sort_values("entry_time").reset_index(drop=True)

    cash = initial_capital
    high_water = initial_capital
    daily_start = initial_capital
    current_day = None
    daily_limit_hit = False
    account_failed = False

    equity_rows: List[Dict[str, Any]] = []

    for _, trade in df.iterrows():
        entry_local = trade["entry_time"].tz_convert("America/New_York")
        exit_local = trade["exit_time"].tz_convert("America/New_York")
        date = entry_local.date()

        if current_day is None or date != current_day:
            current_day = date
            daily_start = cash
            daily_limit_hit = False

        if account_failed:
            break
        if daily_limit_hit:
            continue

        pnl = trade["pnl"] * point_value
        prospective_cash = cash + pnl
        prospective_high_water = max(high_water, prospective_cash)

        daily_breached = (
            prospective_cash < daily_start - TOPSTEP_DAILY_DD
        )
        trailing_breached = (
            prospective_cash < prospective_high_water - TOPSTEP_TRAILING_DD
        )

        if daily_breached:
            daily_limit_hit = True
            continue

        if trailing_breached:
            # Trade executes and blows the account, then capital resets.
            cash = prospective_cash
            equity_rows.append({"date": exit_local.normalize(), "equity": cash})
            cash = initial_capital
            high_water = initial_capital
            daily_start = initial_capital
            daily_limit_hit = False
            continue

        # Normal execution.
        cash = prospective_cash
        high_water = prospective_high_water
        equity_rows.append({"date": exit_local.normalize(), "equity": cash})

    if not equity_rows:
        return pd.DataFrame({"date": [], "equity": []}).set_index("date")

    eq_df = pd.DataFrame(equity_rows).set_index("date")
    daily = eq_df.groupby(eq_df.index)["equity"].last().sort_index()
    full_idx = pd.date_range(daily.index.min(), daily.index.max(), freq="D", tz="America/New_York")
    daily = daily.reindex(full_idx, method="ffill")
    return daily.to_frame("equity")


# ---------------------------------------------------------------------------
# Rust engine summary wrapper
# ---------------------------------------------------------------------------
def run_rust_summary(signals: pd.DataFrame, point_value: float) -> Tuple[Dict[str, Any], pd.DataFrame]:
    """Run finalized signals through the Rust engine and return summary + equity."""
    engine = AgnosticBacktest(cash=INITIAL_CAPITAL, point_value=point_value)
    topstep = TopstepConfig(
        enabled=True,
        daily_drawdown_limit=TOPSTEP_DAILY_DD,
        trailing_drawdown_limit=TOPSTEP_TRAILING_DD,
        profit_target=TOPSTEP_PROFIT_TARGET,
        reset_on_failure=True,
    )
    summary = engine.run_signals(signals, topstep=topstep)

    # Build a matching daily equity curve in Python (Rust run_signals does not
    # expose its internal timestamps through the Python wrapper).
    equity = simulate_topstep_equity(signals, point_value)

    stats = {
        "win_rate": summary.win_rate,
        "n_trades": summary.n_trades,
        "executed_trades": summary.executed_trades,
        "account_blowups": summary.account_blowups,
        "daily_limit_hits": summary.daily_limit_hits,
        "trailing_limit_hits": summary.trailing_limit_hits,
        "max_drawdown": summary.max_drawdown * INITIAL_CAPITAL,
        "sharpe": summary.sharpe,
        "total_pnl": summary.total_pnl,
        "final_equity": summary.final_equity,
        "avg_per_trade": summary.avg_trade,
        "avg_per_week": summary.avg_per_week,
        "avg_per_day": summary.avg_per_day,
        "trades_per_day": summary.n_trades / 10.0 / 252.0,
        "discrete_trades_per_day": _discrete_trades_per_day(signals),
        "profit_target_reached": summary.profit_target_reached,
        "account_failed": summary.account_failed,
    }
    return stats, equity


def run_payout_summary(signals: pd.DataFrame, account_spec: str, contracts: int) -> Tuple[Dict[str, Any], pd.DataFrame]:
    """Run finalized signals through the Topstep payout simulator."""
    if signals.empty:
        return {}, pd.DataFrame({"date": [], "equity": []}).set_index("date")

    trades = signals[["entry_time", "exit_time", "pnl", "instrument"]].copy()
    result = simulate_topstep_payouts(trades, account_spec, contracts=contracts, daily_profit_cap_mode=False)

    equity = result["daily_equity"].to_frame("equity")
    stats = {
        "avg_payout_per_week": result["avg_payout_per_week"],
        "total_payouts_dollars": result["total_payouts_dollars"],
        "payouts_count": result["payouts_count"],
        "failed_combines": result["failed_combines"],
        "resets": result["resets"],
        "daily_limit_hits": result["daily_limit_hits"],
        "consistency_violations": result["consistency_violations"],
        "winning_days": result["winning_days"],
        "win_rate": result["win_rate"] / 100.0,
        "trades_executed": result["trades_executed"],
        "final_equity": result["final_equity"],
        "max_drawdown_pct": result["max_drawdown_pct"],
        "avg_trade_pnl": result["avg_trade_pnl"],
    }
    return stats, equity


def _daily_equity_from_engine(engine: AgnosticBacktest, signals: pd.DataFrame) -> pd.DataFrame:
    """Reconstruct daily account equity using the Rust engine's closed trades."""
    if signals.empty:
        return pd.DataFrame(
            columns=["date", "equity"]
        ).set_index("date")

    curve = list(engine._engine.equity_curve())
    trades = list(engine._engine.trades())
    # equity_curve[0] is starting cash; each subsequent value corresponds to a closed trade.
    if len(curve) != len(trades) + 1:
        # Fallback: use signal exit times with the final equity series.
        pass

    rows = []
    for (entry_ns, exit_ns, entry_px, exit_px, qty, pnl), eq in zip(trades, curve[1:]):
        exit_dt = pd.Timestamp(exit_ns, tz="UTC").tz_convert("America/New_York")
        rows.append({"date": exit_dt.normalize(), "equity": eq})

    if not rows:
        return pd.DataFrame({"date": [], "equity": []}).set_index("date")

    df = pd.DataFrame(rows).set_index("date")
    daily = df.groupby(df.index)["equity"].last().sort_index()
    # Forward-fill across non-trading days to create a continuous calendar index.
    full_idx = pd.date_range(daily.index.min(), daily.index.max(), freq="D", tz="America/New_York")
    daily = daily.reindex(full_idx, method="ffill")
    return daily.to_frame("equity")


# ---------------------------------------------------------------------------
# Portfolio selection from sweep CSV
# ---------------------------------------------------------------------------
def load_sweep_metadata(path: Path) -> pd.DataFrame:
    return pd.read_csv(path)


def enrich_portfolio(portfolio: List[Dict[str, Any]], sweep: pd.DataFrame) -> List[Dict[str, Any]]:
    """Add avg_per_week, win_rate, sharpe from the sweep CSV to each leg."""
    sweep = sweep.copy()
    sweep["strategy_id"] = sweep["strategy_id"].astype(int)
    enriched = []
    for leg in portfolio:
        sid = int(leg["strategy_id"])
        row = sweep[
            (sweep["strategy_id"] == sid)
            & (sweep["instrument"] == leg["instrument"])
            & (sweep["session"] == leg["session"])
            & (sweep["tp"] == leg["tp"])
            & (sweep["sl"] == leg["sl"])
            & (sweep["mode"] == leg["mode"])
        ]
        if row.empty:
            raise ValueError(f"Leg not found in sweep: {leg}")
        r = row.iloc[0]
        enriched.append(
            {
                **leg,
                "strategy_id": sid,
                "avg_per_week": float(r["avg_per_week"]),
                "win_rate": float(r["win_rate"]),
                "sharpe": float(r["sharpe"]),
                "trades_per_day": float(r["trades_per_day"]),
                "max_drawdown": float(r["max_drawdown"]),
            }
        )
    return enriched


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def _discrete_trades_per_day(signals: pd.DataFrame) -> float:
    """Count unique (entry minute, leg) trades, normalized to trading days."""
    if signals.empty:
        return 0.0
    df = signals.copy()
    df["entry_minute"] = pd.to_datetime(df["entry_time"], utc=True).dt.floor("min")
    unique = df.groupby(["entry_minute", "leg"]).size().reset_index()
    return len(unique) / 10.0 / 252.0


def fmt_money(x: float) -> str:
    return f"${x:,.0f}"


def print_payout_report(results: Dict[str, Tuple[Dict[str, Any], pd.DataFrame]]) -> None:
    print("\n" + "=" * 70)
    print("STACK SIMULATOR RESULTS — PAYOUT MODE")
    print("=" * 70)
    for name in sorted(results):
        stats, _ = results[name]
        if not stats:
            continue
        print(f"\n{name}")
        print("-" * len(name))
        print(f"  avg payout/week        {fmt_money(stats['avg_payout_per_week'])}")
        print(f"  total payouts          {fmt_money(stats['total_payouts_dollars'])}")
        print(f"  payouts count          {stats['payouts_count']}")
        print(f"  failed combines        {stats['failed_combines']} ({stats['failed_combines']/10:.1f}/yr)")
        print(f"  resets                 {stats['resets']}")
        print(f"  daily limit hits       {stats['daily_limit_hits']}")
        print(f"  consistency violations {stats['consistency_violations']}")
        print(f"  winning days           {stats['winning_days']}")
        print(f"  win rate               {stats['win_rate']*100:5.2f}%")
        print(f"  trades executed        {stats['trades_executed']}")
        print(f"  final equity           {fmt_money(stats['final_equity'])}")
    print("\n" + "=" * 70)


def print_report(results: Dict[str, Tuple[Dict[str, Any], pd.DataFrame]]) -> None:
    print("\n" + "=" * 70)
    print("STACK SIMULATOR RESULTS (2-contract cap, NQ legs)")
    print("=" * 70)
    for name in sorted(results):
        stats, _ = results[name]
        realistic_weekly = (stats["final_equity"] - INITIAL_CAPITAL) / 10.0 / 52.0
        print(f"\n{name}")
        print("-" * len(name))
        print(f"  avg/week (sweep-style) {fmt_money(stats['avg_per_week'])}")
        print(f"  realistic avg/week     {fmt_money(realistic_weekly)}")
        print(f"  total PnL              {fmt_money(stats['total_pnl'])}")
        print(f"  final equity           {fmt_money(stats['final_equity'])}")
        print(f"  win rate               {stats['win_rate']*100:5.2f}%")
        print(f"  trades/day             {stats['discrete_trades_per_day']:.3f} ({stats['trades_per_day']:.3f} contract-rows)")
        print(f"  max drawdown           {fmt_money(stats['max_drawdown'])}")
        print(f"  account blowups        {stats['account_blowups']} ({stats['account_blowups']/10:.1f}/yr)")
        print(f"  daily limit hits       {stats['daily_limit_hits']}")
        print(f"  trailing hits          {stats['trailing_limit_hits']}")
        print(f"  Sharpe                 {stats['sharpe']:.2f}")
        print(f"  profit target hit      {stats['profit_target_reached']}")
        print(f"  account failed         {stats['account_failed']}")

    print("\n" + "=" * 70)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Stack simulator for Topstep NQ legs.")
    parser.add_argument("--sweep-csv", default=str(SWEEP_CSV), help="Path to paper1 rust sweep CSV")
    parser.add_argument("--output", default=str(OUTPUT_CSV), help="Output CSV path")
    parser.add_argument("--start-date", default="2016-06-01")
    parser.add_argument("--end-date", default="2026-05-29")
    parser.add_argument("--max-contracts", type=int, default=2)
    parser.add_argument("--account", default=None, choices=list(TOPSTEP_SPECS.keys()), help="Enable payout-mode with this Topstep account spec")
    parser.add_argument("--contracts", type=int, default=None, help="Contracts per trade in payout mode (defaults to account max)")
    parser.add_argument("--payout-mode", action="store_true", help="Use Topstep payout simulator instead of Rust engine")
    args = parser.parse_args(argv)

    sweep = load_sweep_metadata(Path(args.sweep_csv))
    portfolio = enrich_portfolio(DEFAULT_PORTFOLIO, sweep)

    print("Portfolio legs:")
    for leg in portfolio:
        print(
            f"  {leg['strategy_id']:>3} {leg['instrument']} {leg['session']:>6} "
            f"tp={leg['tp']} sl={leg['sl']} {leg['mode']:<20} "
            f"$wk={leg['avg_per_week']:>8.0f} WR={leg['win_rate']*100:5.1f}%"
        )

    print("\nGenerating per-leg trades...")
    leg_trades = [generate_leg_trades(leg, args.start_date, args.end_date) for leg in portfolio]
    for leg, trades in zip(portfolio, leg_trades):
        label = f"{leg['strategy_id']:03d}_{leg['instrument']}_{leg['session']}_{leg['mode']}"
        print(f"  {label}: {len(trades)} trades")

    # Metadata for allocation rules
    leg_meta = [
        {
            "priority": leg["avg_per_week"],
            "conviction": leg["sharpe"],  # Rule C uses Sharpe as conviction
        }
        for leg in portfolio
    ]

    point_value = POINT_VALUES["NQ"]
    results: Dict[str, Tuple[Dict[str, Any], pd.DataFrame]] = {}

    # Determine runner function based on payout mode.
    if args.payout_mode:
        account_spec = args.account or "50k_standard"
        contracts = args.contracts if args.contracts is not None else TOPSTEP_SPECS[account_spec]["max_contracts"]
        runner = lambda signals: run_payout_summary(signals, account_spec, contracts)
    else:
        runner = lambda signals: run_rust_summary(signals, point_value)

    # Single best leg at max contracts (or 2 in legacy mode)
    best_leg = max(portfolio, key=lambda x: x["avg_per_week"])
    best_idx = portfolio.index(best_leg)
    single_contracts = args.contracts if args.payout_mode else 2
    single_signals = leg_trades[best_idx].copy()
    single_signals = pd.concat([single_signals] * single_contracts, ignore_index=True)
    single_signals = single_signals.sort_values("entry_time").reset_index(drop=True)
    print(f"\nRunning single best leg ({single_contracts} contracts)...")
    results[f"single_best_leg_{single_contracts}c"] = runner(single_signals)

    # Multi-leg stack rules
    for rule in ("A", "B", "C"):
        print(f"\nRunning multi-leg stack Rule {rule}...")
        stacked = apply_stack_rule(leg_trades, leg_meta, rule, args.max_contracts)
        results[f"stack_rule_{rule}"] = runner(stacked)

    if args.payout_mode:
        print_payout_report(results)
    else:
        print_report(results)

    # Build daily equity CSV
    equity_frames = []
    for name, (_, eq) in results.items():
        col = name.lower().replace(" ", "_")
        frame = eq[["equity"]].rename(columns={"equity": col})
        equity_frames.append(frame)

    equity_df = pd.concat(equity_frames, axis=1)
    equity_df.index = pd.to_datetime(equity_df.index)
    equity_df = equity_df.sort_index()
    equity_df.to_csv(args.output)
    print(f"\nDaily equity written to: {args.output}")
    print(f"Rows: {len(equity_df)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
