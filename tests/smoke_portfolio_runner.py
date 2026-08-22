# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Smoke test for the portfolio_runner module.
#   - Runs strategy 003 and 009 as 9-leg portfolios over a few months and
#     prints portfolio-level stats.
# WHY: Verify the portfolio runner loads data, generates signals, and applies
#      account-level Topstep rules end-to-end without running a full 10-year sweep.

#!/usr/bin/env python3
"""Smoke test for the combined portfolio runner."""
from __future__ import annotations

import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from engine_rust.scripts.portfolio_runner import run_portfolio


def main() -> int:
    data_dir = str(_PROJECT_ROOT / "data")
    for sid in ["003", "009"]:
        result = run_portfolio(
            sid=sid,
            tp=0.5,
            sl=6.0,
            mode_name="breakeven_0.5x",
            base_kwargs={"mode": "breakeven_then_trail", "trail_atr_mult": 0.5},
            start_date="2024-01-01",
            end_date="2024-03-31",
            data_dir=data_dir,
            topstep_enabled=True,
        )
        print(f"--- Portfolio smoke: strategy {sid} ---")
        print(f"  legs={result.n_legs}  trades={result.n_trades}  win_rate={result.win_rate:.1%}")
        print(f"  total_pnl=${result.total_pnl_dollars:,.0f}  avg/week=${result.avg_per_week:,.0f}")
        print(f"  max_dd=${result.max_drawdown:,.0f}  sharpe={result.sharpe:.2f}")
        print(f"  blowups={result.account_blowups}  daily_hits={result.daily_limit_hits}  trailing_hits={result.trailing_limit_hits}")
        print(f"  profit_target={result.profit_target_reached}  account_failed={result.account_failed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
