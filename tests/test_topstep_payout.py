# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created deterministic tests for topstep_payout.py.
#   - Covers: quick profit-target payout, 40% consistency-rule block,
#     and trailing-drawdown reset.

import pandas as pd
import pytest

from topstep_payout import simulate_topstep_payouts, TOPSTEP_SPECS


def _make_trades(rows: list[tuple]) -> pd.DataFrame:
    """Helper: rows are (day, instrument, pnl_points)."""
    records = []
    for i, (day, instrument, pnl) in enumerate(rows):
        t = pd.Timestamp(f"2026-01-{day:02d} 10:00:00")
        records.append(
            {
                "entry_time": t,
                "exit_time": t + pd.Timedelta(minutes=5),
                "instrument": instrument,
                "pnl": pnl,
            }
        )
    return pd.DataFrame(records)


def test_hits_profit_target_and_pays_out():
    """Steady small winners reach PT and min winning days -> payout."""
    # NQ net per 10-point win (1 contract) = 10*20 - 0.5*20 - 5 = 185
    rows = []
    for day in range(1, 6):  # 5 winning days
        for _ in range(4):  # 4 trades/day -> $740/day
            rows.append((day, "NQ", 10))
    df = _make_trades(rows)

    result = simulate_topstep_payouts(df, "50k_standard", contracts=1)

    # 5 days * 4 trades * $185 = $3,700 profit -> cash $53,700 before payout
    # payout = min(53700*0.40, 4000) = $4,000
    assert result["payouts_count"] == 1
    assert result["total_payouts_dollars"] == pytest.approx(4000.0, abs=0.01)
    assert result["winning_days"] == 5
    assert result["consistency_violations"] == 0
    assert result["failed_combines"] == 0
    assert result["resets"] == 1  # payout triggers account reset
    assert result["final_equity"] == pytest.approx(50000.0, abs=0.01)
    assert result["trades_executed"] == 20


def test_consistency_rule_blocks_payout():
    """One outsized winning day violates 40% rule and blocks payout."""
    rows = [(1, "NQ", 250)]  # net = 250*20 - 10 - 5 = 4985; huge vs total
    # Add small winning days to satisfy min_winning_days and PT
    for day in range(2, 6):
        rows.append((day, "NQ", 3))  # net = 3*20 - 10 - 5 = 45
    df = _make_trades(rows)

    result = simulate_topstep_payouts(df, "50k_standard", contracts=1)

    assert result["payouts_count"] == 0
    assert result["total_payouts_dollars"] == pytest.approx(0.0, abs=0.01)
    assert result["consistency_violations"] == 1
    assert result["winning_days"] == 5
    # Cash: 50000 + 4985 + 4*45 = 50165? Wait: 50000+4985=54985, +180=55165
    assert result["final_equity"] == pytest.approx(55165.0, abs=0.01)


def test_trailing_drawdown_resets_account():
    """A loss breaches trailing drawdown (but not daily dd) and resets the combine."""
    # Day 1: push high_water well above the next day's starting cash.
    #   NQ +100 pts -> net 1985, cash 51985, HW 51985
    #   NQ -50  pts -> net -1015, cash 50970, HW still 51985
    # Day 2: YM -195 pts -> net = -195*5 - 15 = -990
    #   cash = 50970 - 990 = 49980
    #   daily limit = 50970 - 1000 = 49970   -> OK
    #   trailing limit = 51985 - 2000 = 49985 -> BREACH -> reset
    # Day 3: small winner on fresh combine.
    rows = [
        (1, "NQ", 100),
        (1, "NQ", -50),
        (2, "YM", -195),
        (3, "YM", 5),  # net = 5*5 - 15 = 10
    ]
    df = _make_trades(rows)

    result = simulate_topstep_payouts(df, "50k_standard", contracts=1)

    assert result["failed_combines"] == 1
    assert result["resets"] == 1
    assert result["payouts_count"] == 0
    assert result["final_equity"] == pytest.approx(50010.0, abs=0.01)
    assert result["trades_executed"] == 4
    assert result["max_drawdown_pct"] > 0.0


def test_daily_profit_cap_blocks_excess_daily_gains():
    """With daily_profit_cap_mode=True, a day is capped at 25% of profit target."""
    # 50k PT = 3000 -> daily cap = 750
    # Each NQ 10-point win nets $185.  5 wins = $925 > 750.
    rows = [(1, "NQ", 10) for _ in range(5)]
    df = _make_trades(rows)

    result = simulate_topstep_payouts(
        df, "50k_standard", contracts=1, daily_profit_cap_mode=True
    )

    # First 4 trades execute: 4*185 = 740.  5th would push to 925 > 750 -> blocked.
    assert result["trades_executed"] == 4
    assert result["profit_cap_blocks"] == 1
    assert result["daily_equity"][pd.Timestamp("2026-01-01").date()] == pytest.approx(
        50740.0, abs=0.01
    )


def test_account_spec_dict_passed_directly():
    """Ensure passing a complete dict works the same as a named spec."""
    spec = TOPSTEP_SPECS["50k_standard"].copy()
    rows = [(1, "NQ", 10) for _ in range(4)]
    df = _make_trades(rows)
    result = simulate_topstep_payouts(df, spec, contracts=1)
    assert result["trades_executed"] == 4
    assert result["final_equity"] == pytest.approx(50740.0, abs=0.01)
