# CHANGE_SUMMARY
# 2026-07-25  backtest-agent
#   - Added unit tests for run_backtest using synthetic signals.
#   - Covers default run, cost subtraction, empty input, and required
#     output keys / structure.
# WHY: Smoke-test the backtest engine without depending on strategy or
#      market-data modules.

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from topstep_strats.backtest import default_params, run_backtest


def _make_signals() -> pd.DataFrame:
    """Three synthetic NQ trades over two calendar days."""
    return pd.DataFrame(
        {
            "entry_time": pd.to_datetime(
                [
                    "2023-01-01 10:00:00",
                    "2023-01-01 11:00:00",
                    "2023-01-02 10:30:00",
                ]
            ),
            "direction": [1, -1, 1],
            "entry_price": [15000.0, 15020.0, 15000.0],
            "stop_loss": [14990.0, 15030.0, 14990.0],
            "take_profit": [15020.0, 15000.0, 15020.0],
            "exit_time": pd.to_datetime(
                [
                    "2023-01-01 10:30:00",
                    "2023-01-01 11:30:00",
                    "2023-01-02 11:00:00",
                ]
            ),
            "exit_price": [15020.0, 15000.0, 14990.0],
            "pnl": [20.0, 20.0, -10.0],
            "exit_reason": ["tp", "tp", "sl"],
        }
    )


def test_default_params():
    p = default_params()
    assert p["initial_capital"] == 100_000.0
    assert p["point_value"] == 20.0
    assert p["slippage"] == 0.0
    assert p["commission"] == 0.0


def test_run_backtest_keys_and_shape():
    signals = _make_signals()
    result = run_backtest(signals)

    assert "trades" in result
    assert "equity_curve" in result
    assert "daily_returns" in result
    assert "start_of_day_to_trough_drawdown" in result
    assert "summary" in result

    assert len(result["trades"]) == 3
    assert len(result["equity_curve"]) == 3
    assert isinstance(result["summary"], dict)


def test_run_backtest_no_costs():
    signals = _make_signals()
    result = run_backtest(signals)
    summary = result["summary"]

    assert summary["total_trades"] == 3
    assert summary["winning_trades"] == 2
    assert summary["losing_trades"] == 1
    assert pytest.approx(summary["win_rate"]) == 2 / 3
    assert pytest.approx(summary["total_pnl_points"]) == 30.0
    assert pytest.approx(summary["net_pnl_points"]) == 30.0
    # 100k + 30 points * $20/point
    assert pytest.approx(summary["final_equity"]) == 100_600.0
    assert pytest.approx(summary["total_return_pct"]) == 600.0 / 100_000.0


def test_run_backtest_costs():
    signals = _make_signals()
    params = {"slippage": 1.0, "commission": 2.0, "point_value": 20.0}
    result = run_backtest(signals, params=params)
    summary = result["summary"]

    cost_per_trade = 3.0
    net = signals["pnl"].sum() - cost_per_trade * len(signals)
    assert pytest.approx(summary["net_pnl_points"]) == net
    assert pytest.approx(result["trades"]["net_pnl"].to_list()) == [
        17.0,
        17.0,
        -13.0,
    ]
    assert pytest.approx(summary["final_equity"]) == 100_000.0 + net * 20.0


def test_daily_returns():
    signals = _make_signals()
    result = run_backtest(signals)
    daily = result["daily_returns"]

    assert isinstance(daily, pd.Series)
    assert len(daily) == 1  # one return from day 1 close -> day 2 close
    # Day 1 close = 100800; Day 2 close = 100600
    assert pytest.approx(daily.iloc[-1]) == (100_600.0 - 100_800.0) / 100_800.0


def test_start_of_day_drawdown():
    signals = _make_signals()
    result = run_backtest(signals)
    sodd = result["start_of_day_to_trough_drawdown"]

    assert len(sodd) == 2
    assert all("date" in r and "drawdown_pct" in r for r in sodd)
    # First day never drops below the open, so SOD drawdown is zero.
    # Second day: open 100800, trough 100600 => -$200 / 100800.
    assert pytest.approx(sodd[0]["drawdown_pct"]) == 0.0
    assert pytest.approx(sodd[1]["drawdown_pct"]) == -200.0 / 100_800.0
    assert pytest.approx(result["summary"]["max_sod_drawdown_pct"]) == -200.0 / 100_800.0


def test_empty_signals():
    result = run_backtest(pd.DataFrame())
    assert result["trades"].empty
    assert result["equity_curve"].empty
    assert result["daily_returns"].empty
    assert result["start_of_day_to_trough_drawdown"] == []
    assert result["summary"]["total_trades"] == 0
    assert result["summary"]["final_equity"] == default_params()["initial_capital"]


def test_missing_columns_raises():
    bad = pd.DataFrame({"entry_time": [1, 2], "pnl": [1, 2]})
    with pytest.raises(ValueError, match="missing required columns"):
        run_backtest(bad)


def test_max_drawdown():
    signals = _make_signals()
    result = run_backtest(signals)
    summary = result["summary"]

    # Peak = 100800 after two winners; trough = 100600 after final loser.
    expected_dd = (100_600.0 - 100_800.0) / 100_800.0
    assert pytest.approx(summary["max_drawdown_pct"]) == expected_dd


def test_topstep_rules_halt_on_daily_limit():
    """A $900 daily drawdown limit should skip trades that breach it."""
    signals = pd.DataFrame(
        {
            "entry_time": pd.to_datetime(
                ["2023-01-01 10:00", "2023-01-01 11:00", "2023-01-01 12:00"]
            ),
            "direction": [-1, -1, -1],
            "entry_price": [15000.0, 15000.0, 15000.0],
            "stop_loss": [15050.0, 15050.0, 15050.0],
            "take_profit": [14950.0, 14950.0, 14950.0],
            "exit_time": pd.to_datetime(
                ["2023-01-01 10:30", "2023-01-01 11:30", "2023-01-01 12:30"]
            ),
            "exit_price": [15050.0, 15050.0, 15050.0],
            "pnl": [-50.0, -50.0, -50.0],
            "exit_reason": ["sl", "sl", "sl"],
        }
    )

    params = {
        "initial_capital": 50_000.0,
        "point_value": 20.0,
        "topstep": {
            "enabled": True,
            "account_size": 50_000.0,
            "daily_drawdown_limit": 900.0,
            "trailing_drawdown_limit": 2_000.0,
            "profit_target": 3_000.0,
        },
    }
    result = run_backtest(signals, params=params)
    summary = result["summary"]

    # Each loser is -50 pts * $20 = -$1000, so the first trade breaches $900 daily limit.
    assert summary["topstep_enabled"] is True
    assert summary["daily_limit_hits"] >= 1
    assert summary["skipped_trades"] >= 1
    assert summary["executed_trades"] < summary["total_trades"]


def test_topstep_daily_limit_resets_next_day():
    """After a daily-limit hit, trading resumes on the next session."""
    # Each -40 pt trade = -$800, so first executes, second breaches $900 daily limit.
    signals = pd.DataFrame(
        {
            "entry_time": pd.to_datetime(
                ["2023-01-01 10:00", "2023-01-01 11:00", "2023-01-02 10:00"]
            ),
            "direction": [-1, -1, 1],
            "entry_price": [15000.0, 15000.0, 15000.0],
            "stop_loss": [15040.0, 15040.0, 14950.0],
            "take_profit": [14960.0, 14960.0, 15050.0],
            "exit_time": pd.to_datetime(
                ["2023-01-01 10:30", "2023-01-01 11:30", "2023-01-02 10:30"]
            ),
            "exit_price": [15040.0, 15040.0, 15050.0],
            "pnl": [-40.0, -40.0, 50.0],
            "exit_reason": ["sl", "sl", "tp"],
        }
    )

    params = {
        "initial_capital": 50_000.0,
        "point_value": 20.0,
        "topstep": {
            "enabled": True,
            "account_size": 50_000.0,
            "daily_drawdown_limit": 900.0,
            "trailing_drawdown_limit": 2_000.0,
            "profit_target": 3_000.0,
        },
    }
    result = run_backtest(signals, params=params)
    trades = result["trades"]

    # First trade executes, second is skipped (daily limit hit), third executes next day.
    assert trades.iloc[0]["skipped"] == False
    assert trades.iloc[1]["skipped"] == True
    assert trades.iloc[2]["skipped"] == False


def test_topstep_profit_target_does_not_halt():
    """Reaching the profit target flags success but keeps trading."""
    signals = pd.DataFrame(
        {
            "entry_time": pd.to_datetime(
                ["2023-01-01 10:00", "2023-01-02 10:00", "2023-01-03 10:00"]
            ),
            "direction": [1, 1, 1],
            "entry_price": [15000.0, 15000.0, 15000.0],
            "stop_loss": [14950.0, 14950.0, 14950.0],
            "take_profit": [15050.0, 15050.0, 15050.0],
            "exit_time": pd.to_datetime(
                ["2023-01-01 10:30", "2023-01-02 10:30", "2023-01-03 10:30"]
            ),
            "exit_price": [15050.0, 15050.0, 15050.0],
            "pnl": [50.0, 50.0, 50.0],
            "exit_reason": ["tp", "tp", "tp"],
        }
    )

    params = {
        "initial_capital": 50_000.0,
        "point_value": 20.0,
        "topstep": {
            "enabled": True,
            "account_size": 50_000.0,
            "daily_drawdown_limit": 900.0,
            "trailing_drawdown_limit": 2_000.0,
            "profit_target": 3_000.0,
        },
    }
    result = run_backtest(signals, params=params)
    summary = result["summary"]

    assert summary["profit_target_reached"] is True
    assert summary["executed_trades"] == summary["total_trades"]
    assert summary["skipped_trades"] == 0
