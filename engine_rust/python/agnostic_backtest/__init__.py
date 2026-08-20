# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Python shim exposing the Rust engine as a pure-Python class.
#   - Accepts a strategy object with `on_bar(self, bar) -> Signal | None`.
# 2026-08-20  kilo
#   - Added Parquet loader, full engine configuration, extended metrics,
#     and timezone-aware session filter kwargs.
# 2026-08-20  kilo
#   - Exposed combine-pass metrics from the Rust Summary on the Python
#     Summary dataclass (passed_combines, failed_combines, combine_pass_rate,
#     days-to-pass percentiles/avg/min/max, trades-to-pass avg/median).
# WHY: Let researchers configure realistic prop-firm constraints from Python
#      and inspect Topstep combine survival statistics after a backtest.

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Protocol, Sequence

import pandas as pd

try:
    from agnostic_backtest._rust import (
        Engine,
        Bar as _RustBar,
        Signal as _RustSignal,
        Summary as _RustSummary,
        TopstepConfig as _RustTopstepConfig,
    )
except ImportError:  # pragma: no cover - wheel not built
    Engine = _RustBar = _RustSignal = _RustSummary = _RustTopstepConfig = None  # type: ignore


@dataclass
class Bar:
    timestamp_ns: int
    open: float
    high: float
    low: float
    close: float
    volume: int


@dataclass
class Signal:
    direction: int  # +1 long, -1 short, 0 flat
    entry_price: float = 0.0
    stop_loss: float = 0.0
    take_profit: float = 0.0


@dataclass
class Summary:
    total_return: float
    n_trades: int
    win_rate: float
    profit_factor: float
    max_drawdown: float
    sharpe: float
    sortino: float = 0.0
    calmar: float = 0.0
    avg_trade: float = 0.0
    avg_per_day: float = 0.0
    avg_per_week: float = 0.0
    hold_time_p50: float = 0.0
    hold_time_p90: float = 0.0
    hold_time_p95: float = 0.0
    failures: int = 0
    final_equity: float = 0.0
    total_pnl: float = 0.0
    executed_trades: int = 0
    daily_limit_hits: int = 0
    trailing_limit_hits: int = 0
    account_failed: bool = False
    account_blowups: int = 0
    profit_target_reached: bool = False
    passed_combines: int = 0
    failed_combines: int = 0
    combine_pass_rate: float = 0.0
    avg_days_to_pass: float = 0.0
    median_days_to_pass: float = 0.0
    min_days_to_pass: int = 0
    max_days_to_pass: int = 0
    days_to_pass_p50: float = 0.0
    days_to_pass_p75: float = 0.0
    days_to_pass_p90: float = 0.0
    days_to_pass_p95: float = 0.0
    days_to_pass_p99: float = 0.0
    avg_trades_to_pass: float = 0.0
    median_trades_to_pass: float = 0.0


@dataclass
class TopstepConfig:
    enabled: bool = False
    daily_drawdown_limit: float = 900.0
    trailing_drawdown_limit: float = 2_000.0
    profit_target: float = 3_000.0
    reset_on_failure: bool = False


class Strategy(Protocol):
    def on_bar(self, bar: Bar) -> Optional[Signal]:
        ...


class AgnosticBacktest:
    """High-performance backtest engine with a Python strategy callback.

    The strategy sees each bar and may return a Signal. Market orders are
    filled at the next bar's open to prevent lookahead bias.

    ``run_signals`` accepts a pre-completed trade schedule and runs it through
    the Rust core for fast PnL accounting and optional Topstep rule checks.
    """

    def __init__(
        self,
        cash: float = 100_000.0,
        commission: float = 0.0,
        slippage: float = 0.0,
        daily_loss_limit: float = 0.0,
        trailing_drawdown: float = 0.0,
        reset_on_failure: bool = True,
        point_value: float = 1.0,
        position_sizing: bool = False,
        timezone: str = "UTC",
        session_start: str | None = None,
        session_end: str | None = None,
    ):
        if Engine is None:
            raise RuntimeError(
                "Rust extension not built. Run: cd engine_rust && maturin develop"
            )
        self._engine = Engine(
            cash,
            commission,
            slippage,
            daily_loss_limit,
            trailing_drawdown,
            reset_on_failure,
            point_value,
            position_sizing,
            timezone,
            session_start,
            session_end,
        )

    def run_csv(self, path: str, strategy: Strategy) -> Summary:
        raw = self._engine.run_csv(path, strategy)
        return self._summary_from_raw(raw)

    def run_parquet(self, path: str, strategy: Strategy) -> Summary:
        raw = self._engine.run_parquet(path, strategy)
        return self._summary_from_raw(raw)

    def run_signals(
        self,
        signals: pd.DataFrame,
        topstep: TopstepConfig | None = None,
    ) -> Summary:
        """Run a finalized signal table through the Rust engine.

        ``signals`` must contain entry_time, exit_time, direction, entry_price,
        exit_price, and pnl columns. PnL is expected in price points and is
        multiplied by the engine's point_value.
        """
        if signals.empty:
            raw = self._engine.run_signals([])
            return self._summary_from_raw(raw)

        df = signals[["entry_time", "exit_time", "direction", "entry_price", "exit_price", "pnl"]].copy()
        entry_ts = pd.to_datetime(df["entry_time"], utc=True).dt.tz_convert("UTC").dt.tz_localize(None)
        exit_ts = pd.to_datetime(df["exit_time"], utc=True).dt.tz_convert("UTC").dt.tz_localize(None)
        # The Rust core expects integer nanoseconds since the Unix epoch.  Some
        # Parquet/CSV loaders return datetime64[ms], so normalize to [ns].
        df["entry_ts"] = entry_ts.astype("datetime64[ns]").astype("int64")
        df["exit_ts"] = exit_ts.astype("datetime64[ns]").astype("int64")
        rows = list(df[["entry_ts", "exit_ts", "direction", "entry_price", "exit_price", "pnl"]].itertuples(index=False, name=None))
        ts = None
        if topstep is not None:
            ts = _RustTopstepConfig(
                topstep.enabled,
                topstep.daily_drawdown_limit,
                topstep.trailing_drawdown_limit,
                topstep.profit_target,
                topstep.reset_on_failure,
            )
        raw = self._engine.run_signals(rows, ts)
        return self._summary_from_raw(raw)

    def _summary_from_raw(self, raw) -> Summary:
        return Summary(
            total_return=raw.total_return,
            n_trades=raw.n_trades,
            win_rate=raw.win_rate,
            profit_factor=raw.profit_factor,
            max_drawdown=raw.max_drawdown,
            sharpe=raw.sharpe,
            sortino=raw.sortino,
            calmar=raw.calmar,
            avg_trade=raw.avg_trade,
            avg_per_day=raw.avg_per_day,
            avg_per_week=raw.avg_per_week,
            hold_time_p50=raw.hold_time_p50,
            hold_time_p90=raw.hold_time_p90,
            hold_time_p95=raw.hold_time_p95,
            failures=raw.failures,
            final_equity=raw.final_equity,
            total_pnl=raw.total_pnl,
            executed_trades=raw.executed_trades,
            daily_limit_hits=raw.daily_limit_hits,
            trailing_limit_hits=raw.trailing_limit_hits,
            account_failed=raw.account_failed,
            account_blowups=raw.account_blowups,
            profit_target_reached=raw.profit_target_reached,
            passed_combines=raw.passed_combines,
            failed_combines=raw.failed_combines,
            combine_pass_rate=raw.combine_pass_rate,
            avg_days_to_pass=raw.avg_days_to_pass,
            median_days_to_pass=raw.median_days_to_pass,
            min_days_to_pass=raw.min_days_to_pass,
            max_days_to_pass=raw.max_days_to_pass,
            days_to_pass_p50=raw.days_to_pass_p50,
            days_to_pass_p75=raw.days_to_pass_p75,
            days_to_pass_p90=raw.days_to_pass_p90,
            days_to_pass_p95=raw.days_to_pass_p95,
            days_to_pass_p99=raw.days_to_pass_p99,
            avg_trades_to_pass=raw.avg_trades_to_pass,
            median_trades_to_pass=raw.median_trades_to_pass,
        )

    def equity_curve(self) -> Sequence[float]:
        return self._engine.equity_curve()

    def trades(self) -> Sequence[tuple]:
        return self._engine.trades()
