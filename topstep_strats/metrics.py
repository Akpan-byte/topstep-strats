# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created topstep_strats/metrics.py implementing the full quantitative
#     metrics suite required by the shared interface.
#   - Added calculate_metrics() consuming the backtest_result dict and
#     returning start-of-day-to-trough drawdown, PSR, DSR, Markov transition
#     strength, Brownian motion / variance-ratio test, Bayesian Sharpe,
#     linear/exponential/quadratic/polynomial regressions on equity, probability
#     of ruin, 20k Monte-Carlo confidence intervals, and 20k bootstrapped CIs.
#   - Vectorized Monte-Carlo and bootstrap loops with NumPy for speed.
#   - Added defensive handling for empty inputs, constant returns, and small
#     samples so metrics degrade gracefully instead of raising.
# WHY: The metrics module is the final analytical step for the TopStep strategy
#      backtest pipeline; it must be fast, reproducible, and robust on both
#      real backtest output and synthetic smoke-test data.

from __future__ import annotations

import warnings
from typing import Any, Dict, List, Optional, Tuple

import numpy as np
import pandas as pd
from scipy import stats
from sklearn.linear_model import LinearRegression
from sklearn.preprocessing import PolynomialFeatures

warnings.filterwarnings("ignore", category=RuntimeWarning)

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def _extract_series(backtest_result: Dict[str, Any], key: str) -> Optional[pd.Series]:
    val = backtest_result.get(key)
    if val is None:
        return None
    if isinstance(val, pd.DataFrame):
        if val.shape[1] == 1:
            return val.iloc[:, 0]
        # If equity curve is in a DataFrame, try a common column name
        if "equity" in val.columns:
            return val["equity"]
        return val.iloc[:, 0]
    return pd.Series(val) if not isinstance(val, pd.Series) else val


def _extract_trades(backtest_result: Dict[str, Any]) -> Optional[pd.DataFrame]:
    trades = backtest_result.get("trades")
    if trades is None or (isinstance(trades, pd.DataFrame) and trades.empty):
        return None
    return trades


def _safe_pct(x: float) -> float:
    return float(x) if np.isfinite(x) else 0.0


def _annualized_sharpe(returns: pd.Series, freq: int = 252, risk_free: float = 0.0) -> float:
    returns = pd.Series(returns).dropna()
    if len(returns) < 2 or returns.std() == 0 or not np.isfinite(returns.std()):
        return 0.0
    mean_excess = returns.mean() - risk_free / freq
    return float(mean_excess / returns.std() * np.sqrt(freq))


def _max_drawdown(equity: pd.Series) -> Tuple[float, int, int]:
    equity = pd.Series(equity).dropna()
    if len(equity) < 2:
        return 0.0, 0, 0
    running_max = equity.cummax()
    drawdown = (equity - running_max) / running_max
    max_dd = drawdown.min()
    if not np.isfinite(max_dd):
        return 0.0, 0, 0
    end_idx = int(drawdown.idxmin()) if isinstance(drawdown.idxmin(), (int, np.integer)) else drawdown.values.argmin()
    start_idx = int(equity.iloc[: end_idx + 1].idxmax()) if isinstance(equity.iloc[: end_idx + 1].idxmax(), (int, np.integer)) else equity.iloc[: end_idx + 1].values.argmax()
    return float(max_dd), start_idx, end_idx


def _cagr(equity: pd.Series, freq: int = 252) -> float:
    equity = pd.Series(equity).dropna()
    if len(equity) < 2 or equity.iloc[0] <= 0 or equity.iloc[-1] <= 0:
        return 0.0
    n_periods = len(equity) / freq
    if n_periods <= 0:
        return 0.0
    return float((equity.iloc[-1] / equity.iloc[0]) ** (1.0 / n_periods) - 1.0)


def _compute_daily_returns(equity: pd.Series) -> pd.Series:
    equity = pd.Series(equity).dropna()
    if len(equity) < 2:
        return pd.Series(dtype=float)
    # Prefer pct_change if index is daily; otherwise treat each step as a day
    return equity.pct_change().dropna()


def _psr_variance(sr: float, skew: float, kurt: float, n: int) -> float:
    # Bailey & López de Prado (2012): variance of the Sharpe ratio estimator
    if n < 2:
        return np.inf
    var = (1.0 - skew * sr + (kurt - 1.0) / 4.0 * sr * sr) / (n - 1.0)
    return max(var, 1e-16)


# ---------------------------------------------------------------------------
# Individual metric functions
# ---------------------------------------------------------------------------


def start_of_day_to_trough_drawdown(
    equity: pd.Series,
    provided: Optional[Any] = None,
) -> Dict[str, Any]:
    """
    Start-of-day to trough drawdown.
    If a value is already provided by the backtester, forward it. Otherwise
    compute the worst peak-to-trough decline using the supplied equity curve.
    """
    if provided is not None:
        if isinstance(provided, dict):
            return provided
        if isinstance(provided, (list, tuple)) and len(provided) == 0:
            pass
        elif isinstance(provided, (list, tuple)) and len(provided) and isinstance(provided[0], dict):
            # Expected format from the backtest engine: list of daily records.
            dds = [d.get("drawdown_pct", 0.0) for d in provided]
            return {"values": dds, "max": float(np.min(dds)) if len(dds) else 0.0}
        else:
            return {"values": list(provided), "max": float(np.min(provided)) if len(provided) else 0.0}

    max_dd, start_idx, end_idx = _max_drawdown(equity)
    return {
        "max": float(max_dd),
        "start_index": int(start_idx),
        "end_index": int(end_idx),
    }


def probabilistic_sharpe_ratio(
    returns: pd.Series,
    sr_benchmark: float = 0.0,
    freq: int = 252,
) -> float:
    """
    Probabilistic Sharpe Ratio (Bailey & López de Prado).
    PSR = Phi( (SR - SR_benchmark) / sigma_SR )
    """
    returns = pd.Series(returns).dropna()
    if len(returns) < 4:
        return 0.0
    sr = _annualized_sharpe(returns, freq=freq)
    skew = float(stats.skew(returns, bias=False))
    kurt = float(stats.kurtosis(returns, fisher=False, bias=False))
    var_sr = _psr_variance(sr, skew, kurt, len(returns))
    if var_sr <= 0 or not np.isfinite(var_sr):
        return 0.0
    z = (sr - sr_benchmark) / np.sqrt(var_sr)
    return float(stats.norm.cdf(z))


def deflated_sharpe_ratio(
    returns: pd.Series,
    n_trials: int = 1,
    freq: int = 252,
) -> float:
    """
    Deflated Sharpe Ratio correcting for selection bias / multiple trials.
    Bailey & López de Prado (2014).
    """
    returns = pd.Series(returns).dropna()
    if len(returns) < 4 or n_trials < 1:
        return 0.0
    sr = _annualized_sharpe(returns, freq=freq)
    skew = float(stats.skew(returns, bias=False))
    kurt = float(stats.kurtosis(returns, fisher=False, bias=False))
    n = len(returns)
    v_sr = np.sqrt(_psr_variance(sr, skew, kurt, n))
    if not np.isfinite(v_sr) or v_sr <= 0:
        return 0.0
    gamma = np.euler_gamma
    term1 = stats.norm.ppf(1.0 - 1.0 / n) if n > 1 else 0.0
    term2 = stats.norm.ppf(1.0 - 1.0 / (n * max(n_trials, 1)))
    sr_star = v_sr * ((1.0 - gamma) * term1 + gamma * term2)
    if not np.isfinite(sr_star):
        return 0.0
    z = (sr - sr_star) / v_sr
    return float(stats.norm.cdf(z))


def markov_transition_strength(returns: pd.Series) -> Dict[str, Any]:
    """
    Estimate first-order Markov transition probabilities on the sign of
    consecutive returns and report a chi-square independence test.
    """
    returns = pd.Series(returns).dropna()
    if len(returns) < 4:
        return {
            "transition_matrix": None,
            "counts": None,
            "chi2": 0.0,
            "pvalue": 1.0,
            "strength": 0.0,
        }
    signs = np.sign(returns).astype(int)
    # Treat zeros as down to keep binary state
    signs[signs == 0] = -1
    current = signs[:-1].values
    nxt = signs[1:].values

    counts = np.zeros((2, 2), dtype=int)
    idx_map = {-1: 0, 1: 1}
    for a, b in zip(current, nxt):
        counts[idx_map[a], idx_map[b]] += 1

    row_totals = counts.sum(axis=1, keepdims=True)
    col_totals = counts.sum(axis=0, keepdims=True)
    total = counts.sum()
    if total == 0 or np.any(row_totals == 0) or np.any(col_totals == 0):
        return {
            "transition_matrix": None,
            "counts": counts.tolist(),
            "chi2": 0.0,
            "pvalue": 1.0,
            "strength": 0.0,
        }

    expected = row_totals @ col_totals / total
    with np.errstate(divide="ignore", invalid="ignore"):
        chi2 = float(np.nansum((counts - expected) ** 2 / expected))
    pvalue = float(1.0 - stats.chi2.cdf(chi2, df=1)) if np.isfinite(chi2) else 1.0

    trans = counts / row_totals
    # Strength: persistence (diag mean) minus reversal (off-diag mean)
    strength = float(np.mean(np.diag(trans)) - np.mean(trans[~np.eye(2, dtype=bool)]))

    return {
        "transition_matrix": trans.tolist(),
        "counts": counts.tolist(),
        "chi2": chi2,
        "pvalue": pvalue,
        "strength": strength,
    }


def brownian_motion_variance_ratio(
    returns: pd.Series,
    q: int = 5,
) -> Dict[str, Any]:
    """
    Variance ratio test for the random-walk / Brownian-motion null.
    Lo & MacKinlay (1988) homoskedastic test statistic.
    """
    returns = pd.Series(returns).dropna()
    if len(returns) < q + 2:
        return {"variance_ratio": 1.0, "z_stat": 0.0, "pvalue": 1.0, "q": q}
    r = returns.values
    mu = np.mean(r)
    var1 = np.var(r, ddof=1)
    if var1 <= 0 or not np.isfinite(var1):
        return {"variance_ratio": 1.0, "z_stat": 0.0, "pvalue": 1.0, "q": q}

    nq = len(r) // q * q
    r_q = np.array([np.sum(r[i : i + q]) for i in range(0, nq - q + 1, q)])
    var_q = np.var(r_q, ddof=1)
    vr = var_q / (q * var1)

    t = len(r)
    denom = np.sqrt(2.0 * (2.0 * q - 1.0) * (q - 1.0) / (3.0 * q * t))
    if denom <= 0:
        z = 0.0
    else:
        z = (vr - 1.0) / denom
    pvalue = float(2.0 * (1.0 - stats.norm.cdf(abs(z)))) if np.isfinite(z) else 1.0

    return {"variance_ratio": float(vr), "z_stat": float(z), "pvalue": pvalue, "q": q}


def bayesian_sharpe(
    returns: pd.Series,
    freq: int = 252,
    n_samples: int = 20000,
    random_state: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Bayesian posterior over the Sharpe ratio assuming normal daily returns
    with a Jeffreys-style prior. Returns posterior mean, median and 95% CI.
    """
    returns = pd.Series(returns).dropna()
    if len(returns) < 4:
        return {"mean": 0.0, "median": 0.0, "ci_95": [0.0, 0.0], "samples": []}
    rng = np.random.default_rng(random_state)
    n = len(returns)
    xbar = returns.mean()
    s2 = returns.var(ddof=1)
    # Posterior: sigma^2 ~ scaled inverse chi-squared; mu | sigma ~ Normal
    sigma2 = (n - 1) * s2 / rng.chisquare(df=n - 1, size=n_samples)
    sigma = np.sqrt(sigma2)
    mu = rng.normal(loc=xbar, scale=sigma / np.sqrt(n), size=n_samples)
    sr_samples = (mu / sigma) * np.sqrt(freq)
    sr_samples = np.nan_to_num(sr_samples, nan=0.0, posinf=0.0, neginf=0.0)
    ci = np.percentile(sr_samples, [2.5, 97.5])
    return {
        "mean": float(np.mean(sr_samples)),
        "median": float(np.median(sr_samples)),
        "ci_95": [float(ci[0]), float(ci[1])],
        "samples": sr_samples.tolist(),
    }


def regress_equity_curve(equity: pd.Series) -> Dict[str, Any]:
    """
    Fit linear, exponential, quadratic and cubic polynomials to the equity curve.
    """
    equity = pd.Series(equity).dropna()
    if len(equity) < 4:
        return {
            "linear": {"slope": 0.0, "intercept": 0.0, "r2": 0.0, "pvalue": 1.0},
            "exponential": {"growth_rate": 0.0, "intercept": 0.0, "r2": 0.0},
            "quadratic": {"coefs": [0.0, 0.0, 0.0], "r2": 0.0},
            "polynomial": {"coefs": [0.0, 0.0, 0.0, 0.0], "r2": 0.0},
        }

    t = np.arange(len(equity), dtype=float)
    y = equity.values.astype(float)
    results: Dict[str, Any] = {}

    # Linear
    slope, intercept, r_val, p_val, _ = stats.linregress(t, y)
    results["linear"] = {
        "slope": float(slope),
        "intercept": float(intercept),
        "r2": float(r_val**2),
        "pvalue": float(p_val),
    }

    # Exponential: log(equity) = a + b*t
    log_y = np.log(y)
    if np.all(np.isfinite(log_y)) and np.all(y > 0):
        b, a, r_val_e, p_val_e, _ = stats.linregress(t, log_y)
        results["exponential"] = {
            "growth_rate": float(b),
            "intercept": float(a),
            "r2": float(r_val_e**2),
        }
    else:
        results["exponential"] = {"growth_rate": 0.0, "intercept": 0.0, "r2": 0.0}

    # Quadratic
    poly2 = np.polyfit(t, y, 2)
    pred2 = np.polyval(poly2, t)
    ss_res2 = np.sum((y - pred2) ** 2)
    ss_tot2 = np.sum((y - np.mean(y)) ** 2)
    results["quadratic"] = {
        "coefs": poly2.tolist(),
        "r2": float(1.0 - ss_res2 / ss_tot2) if ss_tot2 > 0 else 0.0,
    }

    # Cubic polynomial
    poly3 = np.polyfit(t, y, 3)
    pred3 = np.polyval(poly3, t)
    ss_res3 = np.sum((y - pred3) ** 2)
    ss_tot3 = np.sum((y - np.mean(y)) ** 2)
    results["polynomial"] = {
        "coefs": poly3.tolist(),
        "r2": float(1.0 - ss_res3 / ss_tot3) if ss_tot3 > 0 else 0.0,
    }

    return results


def probability_of_ruin(
    returns: pd.Series,
    equity: pd.Series,
    n_sims: int = 20000,
    horizon: Optional[int] = None,
    ruin_fraction: float = 0.0,
    random_state: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Monte-Carlo estimate of the probability that the equity curve falls to or
    below a ruin fraction (default zero) of its starting value.
    """
    returns = pd.Series(returns).dropna()
    equity = pd.Series(equity).dropna()
    if len(returns) < 2 or len(equity) < 2:
        return {"probability": 0.0, "median_drawdown": 0.0, "n_sims": n_sims}
    rng = np.random.default_rng(random_state)
    start = float(equity.iloc[0])
    if horizon is None:
        horizon = len(returns)
    r = returns.values
    idx = rng.integers(0, len(r), size=(n_sims, horizon))
    sim_rets = r[idx]
    sim_equity = start * np.cumprod(1.0 + sim_rets, axis=1)
    ruined = np.any(sim_equity <= ruin_fraction * start, axis=1)
    min_equity = np.min(sim_equity, axis=1)
    return {
        "probability": float(np.mean(ruined)),
        "median_drawdown": float(np.median((min_equity - start) / start)),
        "n_sims": n_sims,
    }


def monte_carlo_simulations(
    returns: pd.Series,
    equity: pd.Series,
    n_sims: int = 20000,
    horizon: Optional[int] = None,
    freq: int = 252,
    random_state: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Vectorized Monte-Carlo simulation of future equity paths by resampling
    historical daily returns. Returns mean terminal wealth, Sharpe, CAGR,
    max-drawdown and 95% confidence intervals.
    """
    returns = pd.Series(returns).dropna()
    equity = pd.Series(equity).dropna()
    if len(returns) < 2 or len(equity) < 2:
        return {
            "terminal_wealth": {"mean": float(equity.iloc[-1]) if len(equity) else 0.0, "ci_95": [0.0, 0.0]},
            "sharpe": {"mean": 0.0, "ci_95": [0.0, 0.0]},
            "cagr": {"mean": 0.0, "ci_95": [0.0, 0.0]},
            "max_drawdown": {"mean": 0.0, "ci_95": [0.0, 0.0]},
            "paths": [],
        }
    rng = np.random.default_rng(random_state)
    start = float(equity.iloc[-1])
    if horizon is None:
        horizon = len(returns)
    r = returns.values
    idx = rng.integers(0, len(r), size=(n_sims, horizon))
    sim_rets = r[idx]
    sim_equity = start * np.cumprod(1.0 + sim_rets, axis=1)

    terminal = sim_equity[:, -1]
    max_dd = np.min(sim_equity / np.maximum.accumulate(sim_equity, axis=1) - 1.0, axis=1)
    sharpe = (np.mean(sim_rets, axis=1) / np.std(sim_rets, axis=1)) * np.sqrt(freq)
    sharpe = np.nan_to_num(sharpe, nan=0.0, posinf=0.0, neginf=0.0)
    cagr = (terminal / start) ** (freq / horizon) - 1.0
    cagr = np.nan_to_num(cagr, nan=0.0, posinf=0.0, neginf=0.0)

    def summarize(arr: np.ndarray) -> Dict[str, Any]:
        arr = np.nan_to_num(arr, nan=0.0, posinf=0.0, neginf=0.0)
        return {
            "mean": float(np.mean(arr)),
            "median": float(np.median(arr)),
            "ci_95": [float(np.percentile(arr, 2.5)), float(np.percentile(arr, 97.5))],
        }

    return {
        "terminal_wealth": summarize(terminal),
        "sharpe": summarize(sharpe),
        "cagr": summarize(cagr),
        "max_drawdown": summarize(max_dd),
        "paths": sim_equity[: min(n_sims, 100)].tolist(),  # keep a small sample for plotting
    }


def bootstrap_metrics(
    returns: pd.Series,
    equity: pd.Series,
    trades: Optional[pd.DataFrame],
    n_boot: int = 20000,
    freq: int = 252,
    random_state: Optional[int] = None,
) -> Dict[str, Any]:
    """
    Vectorized block-bootstrap of daily returns / trade PnLs to obtain
    confidence intervals for Sharpe, CAGR, max drawdown and win rate.
    """
    returns = pd.Series(returns).dropna()
    equity = pd.Series(equity).dropna()
    rng = np.random.default_rng(random_state)

    if len(returns) < 4:
        return {
            "sharpe": {"mean": 0.0, "ci_95": [0.0, 0.0]},
            "cagr": {"mean": 0.0, "ci_95": [0.0, 0.0]},
            "max_drawdown": {"mean": 0.0, "ci_95": [0.0, 0.0]},
            "win_rate": {"mean": 0.0, "ci_95": [0.0, 0.0]},
        }

    r = returns.values
    n = len(r)
    idx = rng.integers(0, n, size=(n_boot, n))
    boot_rets = r[idx]

    means = np.mean(boot_rets, axis=1)
    stds = np.std(boot_rets, axis=1)
    sharpes = np.where(stds > 0, means / stds * np.sqrt(freq), 0.0)

    # Build bootstrapped equity curves
    start = float(equity.iloc[0])
    boot_equity = start * np.cumprod(1.0 + boot_rets, axis=1)
    running_max = np.maximum.accumulate(boot_equity, axis=1)
    boot_dd = np.min((boot_equity - running_max) / running_max, axis=1)

    periods = n / freq
    cagrs = np.where(boot_equity[:, -1] > 0, (boot_equity[:, -1] / start) ** (1.0 / periods) - 1.0, 0.0)

    if trades is not None and "pnl" in trades.columns and len(trades) > 0:
        pnl = trades["pnl"].values
        pnl_idx = rng.integers(0, len(pnl), size=(n_boot, len(pnl)))
        boot_pnl = pnl[pnl_idx]
        win_rates = np.mean(boot_pnl > 0, axis=1)
    else:
        win_rates = np.mean(boot_rets > 0, axis=1)

    def summarize(arr: np.ndarray) -> Dict[str, Any]:
        arr = np.nan_to_num(arr, nan=0.0, posinf=0.0, neginf=0.0)
        return {
            "mean": float(np.mean(arr)),
            "median": float(np.median(arr)),
            "ci_95": [float(np.percentile(arr, 2.5)), float(np.percentile(arr, 97.5))],
        }

    return {
        "sharpe": summarize(sharpes),
        "cagr": summarize(cagrs),
        "max_drawdown": summarize(boot_dd),
        "win_rate": summarize(win_rates),
    }


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------


def calculate_metrics(
    backtest_result: Dict[str, Any],
    n_mc: int = 20000,
    n_boot: int = 20000,
    random_state: int = 42,
) -> Dict[str, Any]:
    """
    Compute the full quantitative metrics suite from a backtest result.

    Parameters
    ----------
    backtest_result : dict
        Must contain at least one of 'equity_curve' or 'trades'. Expected keys:
        - trades: DataFrame with a 'pnl' column.
        - equity_curve: Series of equity values.
        - daily_returns: Series of daily percentage returns (optional).
        - start_of_day_to_trough_drawdown: pre-computed drawdown (optional).
        - summary: dict with auxiliary stats (optional).
    n_mc : int
        Number of Monte-Carlo paths.
    n_boot : int
        Number of bootstrap samples.
    random_state : int
        Seed for reproducibility.

    Returns
    -------
    dict
        Nested dictionary of metrics.
    """
    equity = _extract_series(backtest_result, "equity_curve")
    daily_returns = _extract_series(backtest_result, "daily_returns")
    trades = _extract_trades(backtest_result)
    summary = backtest_result.get("summary", {}) or {}
    provided_dd = backtest_result.get("start_of_day_to_trough_drawdown")

    # Reconstruct equity curve from trades if not provided
    if equity is None or equity.empty:
        if trades is not None and "pnl" in trades.columns:
            equity = pd.Series(np.cumsum(trades["pnl"].values) + 1.0)
        else:
            equity = pd.Series([1.0])

    # Reconstruct daily returns from equity if not provided
    if daily_returns is None or daily_returns.empty:
        daily_returns = _compute_daily_returns(equity)

    if len(daily_returns) < 2 and len(equity) >= 2:
        daily_returns = _compute_daily_returns(equity)

    trades_pnl = trades["pnl"] if trades is not None and "pnl" in trades.columns else pd.Series(dtype=float)

    # Basic diagnostics
    max_dd, dd_start, dd_end = _max_drawdown(equity)
    sharpe = _annualized_sharpe(daily_returns)
    cagr = _cagr(equity)
    win_rate = float(np.mean(trades_pnl > 0)) if len(trades_pnl) > 0 else 0.0
    total_return = float(equity.iloc[-1] / equity.iloc[0] - 1.0) if len(equity) > 1 and equity.iloc[0] > 0 else 0.0

    n_trials = summary.get("n_trials", 1)
    horizon = summary.get("horizon", max(len(daily_returns), 252))

    metrics: Dict[str, Any] = {
        "basic": {
            "start_equity": float(equity.iloc[0]) if len(equity) else 0.0,
            "end_equity": float(equity.iloc[-1]) if len(equity) else 0.0,
            "total_return": total_return,
            "cagr": cagr,
            "sharpe_ratio": sharpe,
            "max_drawdown": max_dd,
            "max_drawdown_start_index": dd_start,
            "max_drawdown_end_index": dd_end,
            "win_rate": win_rate,
            "n_trades": int(len(trades_pnl)),
            "n_days": int(len(daily_returns)),
        },
        "start_of_day_to_trough_drawdown": start_of_day_to_trough_drawdown(equity, provided_dd),
        "probabilistic_sharpe_ratio": probabilistic_sharpe_ratio(daily_returns),
        "deflated_sharpe_ratio": deflated_sharpe_ratio(daily_returns, n_trials=n_trials),
        "markov_transition_strength": markov_transition_strength(daily_returns),
        "brownian_motion_test": brownian_motion_variance_ratio(daily_returns),
        "bayesian_sharpe": bayesian_sharpe(daily_returns, random_state=random_state),
        "regressions": regress_equity_curve(equity),
        "probability_of_ruin": probability_of_ruin(daily_returns, equity, n_sims=n_mc, random_state=random_state),
        "monte_carlo": monte_carlo_simulations(
            daily_returns, equity, n_sims=n_mc, horizon=horizon, random_state=random_state
        ),
        "bootstrap": bootstrap_metrics(
            daily_returns, equity, trades, n_boot=n_boot, random_state=random_state
        ),
    }

    return metrics
