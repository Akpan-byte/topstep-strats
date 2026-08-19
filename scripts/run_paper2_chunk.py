# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created run_paper2_chunk.py, the Paper-2 per-cell entry point.
#   - Takes strategy-id, instrument, session, start/end dates, output path, and
#     an optional data path.
#   - Resolves the strategy configuration from topstep_strats.paper2_registry,
#     loads the requested instrument data (CSV or parquet), filters to the
#     session and date chunk, and runs both raw and Topstep-constrained backtests.
#   - Emits a JSON artifact matching the structure expected by aggregate.py:
#     top-level strategy/start_date/end_date/params plus raw and topstep reports.
# WHY: Paper-2 needs a matrix-style runner that crosses 108 strategy variants
#      with instruments, sessions, and date chunks while reusing the existing
#      backtest/metrics/aggregation pipeline.

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict


_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from topstep_strats.backtest import run_backtest
from topstep_strats.data import get_session_mask, load_market_data, split_by_date
from topstep_strats.metrics import calculate_metrics
from topstep_strats.paper2_registry import (
    INSTRUMENT_CONFIG,
    SESSION_CONFIG,
    get_strategy_config,
    make_strategy_key,
)
from topstep_strats.strategies.paper2_matrix import generate_signals


def _final_equity(backtest_result: Dict[str, Any]) -> float:
    ec = backtest_result.get("equity_curve")
    if ec is None:
        return 0.0
    try:
        if hasattr(ec, "iloc"):
            return float(ec.iloc[-1]) if len(ec) else 0.0
        return float(ec[-1]) if len(ec) else 0.0
    except Exception:
        return 0.0


def _extract_metrics_summary(metrics: Dict[str, Any]) -> Dict[str, Any]:
    """Pull scalar values from the full quant-suite metrics dict.

    Mirrors scripts/run_chunk.py so aggregate.py can consume the output
    without changes.
    """
    if not metrics:
        return {}

    def _get(path, default=0.0):
        node = metrics
        for key in path:
            if isinstance(node, dict):
                node = node.get(key, default)
            else:
                return default
        return node

    def _ci(path):
        val = _get(path, [0.0, 0.0])
        return list(val) if isinstance(val, (list, tuple)) else [0.0, 0.0]

    return {
        "psr": float(_get(["probabilistic_sharpe_ratio"], 0.0)),
        "dsr": float(_get(["deflated_sharpe_ratio"], 0.0)),
        "markov_strength": float(_get(["markov_transition_strength", "strength"], 0.0)),
        "markov_chi2": float(_get(["markov_transition_strength", "chi2"], 0.0)),
        "markov_pvalue": float(_get(["markov_transition_strength", "pvalue"], 1.0)),
        "brownian_vr": float(_get(["brownian_motion_test", "variance_ratio"], 1.0)),
        "brownian_z": float(_get(["brownian_motion_test", "z_stat"], 0.0)),
        "brownian_pvalue": float(_get(["brownian_motion_test", "pvalue"], 1.0)),
        "bayesian_sharpe_mean": float(_get(["bayesian_sharpe", "mean"], 0.0)),
        "bayesian_sharpe_median": float(_get(["bayesian_sharpe", "median"], 0.0)),
        "bayesian_sharpe_ci95": _ci(["bayesian_sharpe", "ci_95"]),
        "linear_r2": float(_get(["regressions", "linear", "r2"], 0.0)),
        "linear_slope": float(_get(["regressions", "linear", "slope"], 0.0)),
        "exponential_r2": float(_get(["regressions", "exponential", "r2"], 0.0)),
        "exponential_growth_rate": float(_get(["regressions", "exponential", "growth_rate"], 0.0)),
        "quadratic_r2": float(_get(["regressions", "quadratic", "r2"], 0.0)),
        "polynomial_r2": float(_get(["regressions", "polynomial", "r2"], 0.0)),
        "probability_of_ruin": float(_get(["probability_of_ruin", "probability"], 0.0)),
        "mc_terminal_wealth_mean": float(_get(["monte_carlo", "terminal_wealth", "mean"], 0.0)),
        "mc_terminal_wealth_ci95": _ci(["monte_carlo", "terminal_wealth", "ci_95"]),
        "mc_sharpe_mean": float(_get(["monte_carlo", "sharpe", "mean"], 0.0)),
        "mc_sharpe_ci95": _ci(["monte_carlo", "sharpe", "ci_95"]),
        "mc_cagr_mean": float(_get(["monte_carlo", "cagr", "mean"], 0.0)),
        "mc_cagr_ci95": _ci(["monte_carlo", "cagr", "ci_95"]),
        "mc_max_drawdown_mean": float(_get(["monte_carlo", "max_drawdown", "mean"], 0.0)),
        "mc_max_drawdown_ci95": _ci(["monte_carlo", "max_drawdown", "ci_95"]),
        "boot_sharpe_mean": float(_get(["bootstrap", "sharpe", "mean"], 0.0)),
        "boot_sharpe_ci95": _ci(["bootstrap", "sharpe", "ci_95"]),
        "boot_cagr_mean": float(_get(["bootstrap", "cagr", "mean"], 0.0)),
        "boot_cagr_ci95": _ci(["bootstrap", "cagr", "ci_95"]),
        "boot_max_drawdown_mean": float(_get(["bootstrap", "max_drawdown", "mean"], 0.0)),
        "boot_max_drawdown_ci95": _ci(["bootstrap", "max_drawdown", "ci_95"]),
        "boot_win_rate_mean": float(_get(["bootstrap", "win_rate", "mean"], 0.0)),
        "boot_win_rate_ci95": _ci(["bootstrap", "win_rate", "ci_95"]),
    }


def _build_report(
    strategy_key: str,
    start_date: str,
    end_date: str,
    backtest_result: Dict[str, Any],
    metrics: Dict[str, Any],
    params: Dict[str, Any],
) -> Dict[str, Any]:
    """Convert a full backtest + metrics result into a small aggregate-ready dict."""
    summary = backtest_result.get("summary", {}) if isinstance(backtest_result, dict) else {}
    metrics = metrics or {}
    basic = metrics.get("basic", {}) if isinstance(metrics, dict) else {}

    return {
        "strategy": strategy_key,
        "start_date": start_date,
        "end_date": end_date,
        "params": params,
        "trades": int(summary.get("total_trades", len(backtest_result.get("trades", [])))),
        "executed_trades": int(summary.get("executed_trades", summary.get("total_trades", 0))),
        "skipped_trades": int(summary.get("skipped_trades", 0)),
        "winning_trades": int(summary.get("winning_trades", 0)),
        "losing_trades": int(summary.get("losing_trades", 0)),
        "win_rate": float(basic.get("win_rate", summary.get("win_rate", 0.0))),
        "avg_trade": float(summary.get("avg_pnl_points", summary.get("avg_trade", 0.0))),
        "profit_factor": float(summary.get("profit_factor", 0.0)),
        "gross_profit": float(summary.get("gross_profit_points", summary.get("gross_profit", 0.0))),
        "gross_loss": float(summary.get("gross_loss_points", summary.get("gross_loss", 0.0))),
        "max_drawdown": float(
            basic.get("max_drawdown", summary.get("max_drawdown_pct", summary.get("max_drawdown", 0.0)))
        ),
        "max_drawdown_dollar": float(summary.get("max_drawdown_dollar", 0.0)),
        "max_sod_drawdown_dollar": float(summary.get("max_sod_drawdown_dollar", 0.0)),
        "avg_sod_drawdown_dollar": float(summary.get("avg_sod_drawdown_dollar", 0.0)),
        "sharpe": float(basic.get("sharpe_ratio", summary.get("sharpe", 0.0))),
        "cagr": float(basic.get("cagr", summary.get("cagr", 0.0))),
        "total_return": float(basic.get("total_return", 0.0)),
        "start_equity": float(basic.get("start_equity", summary.get("initial_capital", 0.0))),
        "final_equity": float(basic.get("end_equity", _final_equity(backtest_result))),
        "metrics_keys": sorted(str(k) for k in metrics.keys()),
        "metrics_summary": _extract_metrics_summary(metrics),
        "topstep_enabled": bool(summary.get("topstep_enabled", False)),
        "daily_limit_hits": int(summary.get("daily_limit_hits", 0)),
        "trailing_limit_hits": int(summary.get("trailing_limit_hits", 0)),
        "account_failed": bool(summary.get("account_failed", False)),
        "profit_target_reached": bool(summary.get("profit_target_reached", False)),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Backtest one Paper-2 cell (strategy-id x instrument x session x chunk).")
    parser.add_argument("--strategy-id", type=int, required=True, help="Index into the Paper-2 strategy registry")
    parser.add_argument(
        "--instrument",
        required=True,
        choices=list(INSTRUMENT_CONFIG.keys()),
        help="Futures instrument (NQ/ES/YM)",
    )
    parser.add_argument(
        "--session",
        required=True,
        choices=list(SESSION_CONFIG.keys()),
        help="Trading session (Asian/London/NY)",
    )
    parser.add_argument("--start-date", required=True, help="Chunk start (YYYY-MM-DD)")
    parser.add_argument("--end-date", required=True, help="Chunk end (YYYY-MM-DD)")
    parser.add_argument("--output", required=True, help="Path to write JSON result")
    parser.add_argument(
        "--data-path",
        default=None,
        help="Path to 1-minute CSV/parquet (default: project data dir)",
    )
    args = parser.parse_args(argv)

    strategy_cfg = get_strategy_config(args.strategy_id)
    blueprint = strategy_cfg["blueprint"]

    instrument_cfg = INSTRUMENT_CONFIG[args.instrument]
    session_cfg = SESSION_CONFIG[args.session]

    data_path = args.data_path
    if data_path is None:
        from topstep_strats.paper2_registry import default_data_path

        data_path = default_data_path(args.instrument)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    df = load_market_data(data_path)

    # Vectorized session mask.  No lookahead: session membership is known from
    # the bar's own timestamp.
    session_mask = get_session_mask(
        df,
        start_time=session_cfg["start"],
        end_time=session_cfg["end"],
        tz=session_cfg["tz"],
    )
    df_session = df.loc[session_mask].copy()

    # Date chunk filter (inclusive).
    df_chunk = split_by_date(df_session, args.start_date, args.end_date)

    # Merge registry params with runtime instrument/session overrides.
    strategy_params = {**strategy_cfg}
    strategy_params["blueprint"] = blueprint
    strategy_params["instrument"] = args.instrument
    strategy_params["session"] = args.session
    strategy_params["session_start"] = session_cfg["start"]
    strategy_params["session_end"] = session_cfg["end"]
    strategy_params["tz"] = session_cfg["tz"]
    strategy_params.setdefault("tick_size", instrument_cfg["tick_size"])
    strategy_params.setdefault("point_value", instrument_cfg["point_value"])

    backtest_params = {"point_value": instrument_cfg["point_value"]}
    # User requested the full quant suite EXCEPT bootstrapped CIs and MC sims.
    metrics_kwargs = {"n_mc": 0, "n_boot": 0}

    params = {
        "strategy_id": args.strategy_id,
        "blueprint": blueprint,
        "instrument": args.instrument,
        "session": args.session,
        "strategy_params": strategy_params,
        "backtest_params": backtest_params,
        "metrics_kwargs": metrics_kwargs,
    }

    strategy_key = make_strategy_key(args.strategy_id, args.instrument, args.session)

    if df_chunk.empty:
        empty_bt = {
            "trades": [],
            "equity_curve": [],
            "summary": {},
        }
        raw_report = _build_report(strategy_key, args.start_date, args.end_date, empty_bt, {}, params)
        topstep_report = _build_report(strategy_key, args.start_date, args.end_date, empty_bt, {}, params)
    else:
        signals = generate_signals(df_chunk, strategy_params)

        raw_bt = run_backtest(signals, backtest_params)
        raw_metrics = calculate_metrics(raw_bt, **metrics_kwargs)
        raw_report = _build_report(strategy_key, args.start_date, args.end_date, raw_bt, raw_metrics, params)

        topstep_bt_params = {**backtest_params, "topstep": {"enabled": True}}
        topstep_bt = run_backtest(signals, topstep_bt_params)
        topstep_metrics = calculate_metrics(topstep_bt, **metrics_kwargs)
        topstep_report = _build_report(
            strategy_key, args.start_date, args.end_date, topstep_bt, topstep_metrics, params
        )

    report = {
        "strategy": strategy_key,
        "instrument": args.instrument,
        "session": args.session,
        "start_date": args.start_date,
        "end_date": args.end_date,
        "params": params,
        "raw": raw_report,
        "topstep": topstep_report,
    }

    with open(output_path, "w") as f:
        json.dump(report, f, indent=2, default=str)

    print(f"[run_paper2_chunk] wrote {output_path}")


if __name__ == "__main__":
    main()
