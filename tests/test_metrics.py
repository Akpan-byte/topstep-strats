# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created tests/test_metrics.py to exercise topstep_strats.metrics.
#   - Builds synthetic backtest results (equity curve, daily returns, trades)
#     and asserts that calculate_metrics returns the required keys and
#     sensible numerical bounds.
#   - Tests cover empty input handling, deterministic seeds, regression fits,
#     and Monte-Carlo / bootstrap CI shapes.
# WHY: Smoke-test the metrics module without running a full 10-year backtest.

from __future__ import annotations

import numpy as np
import pandas as pd
import pytest

from topstep_strats.metrics import calculate_metrics


def _synthetic_backtest(n_days: int = 252, seed: int = 42) -> dict:
    """Create a synthetic backtest result with a slight upward drift."""
    rng = np.random.default_rng(seed)
    daily_returns = rng.normal(loc=0.0004, scale=0.015, size=n_days)
    equity_values = np.cumprod(1.0 + daily_returns)
    equity_values = equity_values / equity_values[0] * 100_000.0
    equity = pd.Series(equity_values)
    # Construct fake trades: one trade per ~2 days
    n_trades = n_days // 2
    pnl = rng.normal(loc=8.0, scale=45.0, size=n_trades)
    trades = pd.DataFrame(
        {
            "entry_time": pd.date_range("2020-01-01", periods=n_trades, freq="2D"),
            "direction": rng.choice([1, -1], size=n_trades),
            "entry_price": rng.uniform(9000.0, 16000.0, size=n_trades),
            "pnl": pnl,
        }
    )
    return {
        "trades": trades,
        "equity_curve": equity,
        "daily_returns": pd.Series(daily_returns),
        "start_of_day_to_trough_drawdown": None,
        "summary": {"n_trials": 1},
    }


class TestMetricsInterface:
    def test_calculate_metrics_returns_required_keys(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)

        required = {
            "basic",
            "start_of_day_to_trough_drawdown",
            "probabilistic_sharpe_ratio",
            "deflated_sharpe_ratio",
            "markov_transition_strength",
            "brownian_motion_test",
            "bayesian_sharpe",
            "regressions",
            "probability_of_ruin",
            "monte_carlo",
            "bootstrap",
        }
        assert required.issubset(metrics.keys()), f"Missing keys: {required - metrics.keys()}"

    def test_basic_metrics_sensible_bounds(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        basic = metrics["basic"]

        assert basic["n_days"] == 252
        assert basic["n_trades"] == 126
        assert 0.0 <= basic["win_rate"] <= 1.0
        assert -1.0 <= basic["max_drawdown"] <= 0.0
        assert basic["start_equity"] == pytest.approx(100_000.0)
        assert basic["end_equity"] > 0.0

    def test_psr_and_dsr_are_probabilities(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)

        assert 0.0 <= metrics["probabilistic_sharpe_ratio"] <= 1.0
        assert 0.0 <= metrics["deflated_sharpe_ratio"] <= 1.0

    def test_markov_transition_structure(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        markov = metrics["markov_transition_strength"]

        assert "transition_matrix" in markov
        assert "counts" in markov
        assert "chi2" in markov
        assert "pvalue" in markov
        assert "strength" in markov
        if markov["transition_matrix"] is not None:
            mat = np.array(markov["transition_matrix"])
            assert mat.shape == (2, 2)
            np.testing.assert_allclose(mat.sum(axis=1), [1.0, 1.0], atol=1e-12)

    def test_brownian_motion_test_fields(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        bmt = metrics["brownian_motion_test"]

        assert "variance_ratio" in bmt
        assert "z_stat" in bmt
        assert "pvalue" in bmt
        assert 0.0 <= bmt["pvalue"] <= 1.0

    def test_bayesian_sharpe_samples(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        bayes = metrics["bayesian_sharpe"]

        assert "mean" in bayes
        assert "median" in bayes
        assert "ci_95" in bayes
        assert len(bayes["ci_95"]) == 2
        assert bayes["ci_95"][0] <= bayes["ci_95"][1]

    def test_regressions_r2_nonnegative(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        regs = metrics["regressions"]

        for name in ("linear", "exponential", "quadratic", "polynomial"):
            assert name in regs
            assert regs[name]["r2"] >= 0.0
        assert regs["linear"]["pvalue"] >= 0.0

    def test_monte_carlo_ci_shape(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        mc = metrics["monte_carlo"]

        for key in ("terminal_wealth", "sharpe", "cagr", "max_drawdown"):
            assert "mean" in mc[key]
            assert "ci_95" in mc[key]
            assert len(mc[key]["ci_95"]) == 2
            assert mc[key]["ci_95"][0] <= mc[key]["ci_95"][1]

    def test_bootstrap_ci_shape(self):
        bt = _synthetic_backtest(n_days=252)
        metrics = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=42)
        boot = metrics["bootstrap"]

        for key in ("sharpe", "cagr", "max_drawdown", "win_rate"):
            assert "mean" in boot[key]
            assert "ci_95" in boot[key]
            assert len(boot[key]["ci_95"]) == 2
            assert boot[key]["ci_95"][0] <= boot[key]["ci_95"][1]

    def test_reproducibility_with_same_seed(self):
        bt = _synthetic_backtest(n_days=252)
        m1 = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=123)
        m2 = calculate_metrics(bt, n_mc=1000, n_boot=1000, random_state=123)
        assert m1["basic"]["sharpe_ratio"] == pytest.approx(m2["basic"]["sharpe_ratio"])
        assert m1["bootstrap"]["sharpe"]["mean"] == pytest.approx(m2["bootstrap"]["sharpe"]["mean"])
        assert m1["monte_carlo"]["sharpe"]["mean"] == pytest.approx(m2["monte_carlo"]["sharpe"]["mean"])

    def test_empty_backtest_graceful(self):
        bt = {
            "trades": pd.DataFrame(columns=["pnl"]),
            "equity_curve": pd.Series([1.0]),
            "daily_returns": pd.Series(dtype=float),
            "summary": {},
        }
        metrics = calculate_metrics(bt, n_mc=100, n_boot=100, random_state=42)
        assert metrics["basic"]["sharpe_ratio"] == 0.0
        assert metrics["basic"]["win_rate"] == 0.0
        assert metrics["basic"]["n_trades"] == 0


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
