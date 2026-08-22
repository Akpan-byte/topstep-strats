# CHANGE_SUMMARY
# 2026-08-20  coder
#   - Smoke test for the portfolio1 combined runner.
#   - Runs a single Paper-1 strategy across NQ/ES/YM and Asian/London/NY for a
#     few months and prints portfolio-level statistics.
# WHY: Quick validation that the portfolio runner loads data, generates signals,
#      aggregates legs, and applies account-level Topstep rules.

from __future__ import annotations

import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from engine_rust.scripts.portfolio_runner import run_portfolio


def test_portfolio_runner_smoke():
    """Run strategy 003 as a 9-leg portfolio over Q1 2024."""
    result = run_portfolio(
        sid="003",
        tp=1.0,
        sl=6.0,
        mode_name="trail_1.0x",
        base_kwargs={"mode": "trail", "trail_atr_mult": 1.0},
        start_date="2024-01-01",
        end_date="2024-03-31",
        data_dir=str(_PROJECT_ROOT / "data"),
        topstep_enabled=True,
    )

    print("\nPortfolio1 smoke test result (strategy 003, Q1 2024):")
    print(f"  legs run      : {result.n_legs}")
    print(f"  total trades  : {result.n_trades}")
    print(f"  win rate      : {result.win_rate:.2%}")
    print(f"  total PnL ($) : {result.total_pnl_dollars:,.2f}")
    print(f"  avg per week  : {result.avg_per_week:,.2f}")
    print(f"  max drawdown  : {result.max_drawdown:,.2f}")
    print(f"  sharpe        : {result.sharpe:.3f}")
    print(f"  blowups       : {result.account_blowups}")
    print(f"  profit target : {result.profit_target_reached}")
    print(f"  account failed: {result.account_failed}")

    assert result.n_legs > 0, "portfolio should run at least one leg"
    assert result.n_trades >= 0
    assert 0.0 <= result.win_rate <= 1.0
    assert result.max_drawdown >= 0.0


def test_portfolio_runner_breakeven_mode():
    """Run strategy 009 with the breakeven_then_trail runner."""
    result = run_portfolio(
        sid="009",
        tp=0.5,
        sl=6.0,
        mode_name="breakeven_0.5x",
        base_kwargs={"mode": "breakeven_then_trail", "trail_atr_mult": 0.5},
        start_date="2024-01-01",
        end_date="2024-03-31",
        data_dir=str(_PROJECT_ROOT / "data"),
        topstep_enabled=True,
    )
    print("\nPortfolio1 breakeven mode result (strategy 009, Q1 2024):")
    print(f"  legs run      : {result.n_legs}")
    print(f"  total trades  : {result.n_trades}")
    print(f"  win rate      : {result.win_rate:.2%}")
    print(f"  total PnL ($) : {result.total_pnl_dollars:,.2f}")
    print(f"  avg per week  : {result.avg_per_week:,.2f}")
    print(f"  max drawdown  : {result.max_drawdown:,.2f}")
    print(f"  blowups       : {result.account_blowups}")

    assert result.n_legs > 0


if __name__ == "__main__":
    test_portfolio_runner_smoke()
    test_portfolio_runner_breakeven_mode()
    print("\nportfolio_runner smoke tests passed")
