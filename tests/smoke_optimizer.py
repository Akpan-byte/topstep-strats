# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Smoke test for the optimizer module.
#   - Runs the greedy optimizer on the existing Paper-1 sweep CSV and prints
#     the top 5 candidate portfolios.
# WHY: Verify the optimizer loads, deduplicates, and ranks portfolios quickly
#      without running a full sweep.

#!/usr/bin/env python3
"""Smoke test for the portfolio optimizer."""
from __future__ import annotations

import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from engine_rust.scripts.optimizer import optimize


def main() -> int:
    csv_path = _PROJECT_ROOT / "gh_results" / "rust_sweep" / "paper1_rust_sweep.csv"
    if not csv_path.exists():
        print(f"SKIP: sweep CSV not found at {csv_path}")
        return 0

    print(f"Running optimizer smoke on {csv_path}")
    candidates, legs, meets_targets = optimize(
        str(csv_path),
        min_weekly=5_000.0,
        min_win_rate=0.80,
        max_size=5,
        n_seeds=50,
        local_search=True,
        top_n=5,
    )
    print(f"Loaded {len(legs)} unique legs")
    print(f"Targets met: {meets_targets}")
    for i, p in enumerate(candidates, 1):
        print(
            f"  {i}. weekly=${p.expected_weekly_pnl:,.0f}  "
            f"win_rate={p.blended_win_rate:.1%}  "
            f"dd_est=${p.drawdown_estimate:,.0f}  "
            f"n_trades={p.n_trades}  legs={len(p.legs)}"
        )
        print(f"     {p.to_dict()['legs']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
