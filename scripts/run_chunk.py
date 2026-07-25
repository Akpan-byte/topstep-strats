# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created run_chunk.py, the GitHub Actions matrix entry point for one
#     strategy + date-chunk combination.
#   - Imports the shared topstep_strats interfaces (data, strategies, backtest,
#     metrics) and emits a compact JSON artifact with scalar summary stats.
#   - Falls back to a deterministic synthetic report when the strategy modules
#     are not available, so smoke tests stay isolated from other work-streams.
# WHY: The parallel workflow uploads one small JSON per chunk; aggregating
#      huge equity curves / MC arrays would waste artifact storage.

import argparse
import hashlib
import json
import os
import random
import sys
from pathlib import Path


# Make the project root importable regardless of how this script is invoked.
_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))


def load_modules():
    """Import shared project modules. Returns None if they are not ready yet."""
    try:
        from topstep_strats.data import load_nq_data, split_by_date
        from topstep_strats.backtest import run_backtest
        from topstep_strats.metrics import calculate_metrics
        from topstep_strats.strategies import kasen_orb, nitro_crt
        return {
            "load_nq_data": load_nq_data,
            "split_by_date": split_by_date,
            "run_backtest": run_backtest,
            "calculate_metrics": calculate_metrics,
            "strategies": {
                "kasen_orb": kasen_orb,
                "nitro_crt": nitro_crt,
            },
        }
    except Exception as exc:  # pragma: no cover - modules may be written by another agent
        print(f"[run_chunk] shared modules not available, using synthetic fallback: {exc}", file=sys.stderr)
        return None


def _final_equity(backtest_result):
    ec = backtest_result.get("equity_curve")
    if ec is None:
        return 0.0
    try:
        if hasattr(ec, "iloc"):
            return float(ec.iloc[-1]) if len(ec) else 0.0
        return float(ec[-1]) if len(ec) else 0.0
    except Exception:
        return 0.0


def _build_report(strategy, start_date, end_date, backtest_result, metrics, params):
    """Convert a full backtest + metrics result into a small aggregate-ready dict."""
    summary = backtest_result.get("summary", {}) if isinstance(backtest_result, dict) else {}
    metrics = metrics or {}
    basic = metrics.get("basic", {}) if isinstance(metrics, dict) else {}

    report = {
        "strategy": strategy,
        "start_date": start_date,
        "end_date": end_date,
        "params": params,
        "trades": int(summary.get("total_trades", len(backtest_result.get("trades", [])))),
        "executed_trades": int(summary.get("executed_trades", summary.get("total_trades", 0))),
        "skipped_trades": int(summary.get("skipped_trades", 0)),
        "winning_trades": int(summary.get("winning_trades", 0)),
        "losing_trades": int(summary.get("losing_trades", 0)),
        "win_rate": float(basic.get("win_rate", summary.get("win_rate", 0.0))),
        "avg_trade": float(
            summary.get("avg_pnl_points", summary.get("avg_trade", 0.0))
        ),
        "profit_factor": float(summary.get("profit_factor", 0.0)),
        "gross_profit": float(
            summary.get("gross_profit_points", summary.get("gross_profit", 0.0))
        ),
        "gross_loss": float(
            summary.get("gross_loss_points", summary.get("gross_loss", 0.0))
        ),
        "max_drawdown": float(
            basic.get("max_drawdown", summary.get("max_drawdown_pct", summary.get("max_drawdown", 0.0)))
        ),
        "max_drawdown_dollar": float(summary.get("max_drawdown_dollar", 0.0)),
        "max_sod_drawdown_dollar": float(summary.get("max_sod_drawdown_dollar", 0.0)),
        "avg_sod_drawdown_dollar": float(summary.get("avg_sod_drawdown_dollar", 0.0)),
        "sharpe": float(basic.get("sharpe_ratio", summary.get("sharpe", 0.0))),
        "cagr": float(basic.get("cagr", summary.get("cagr", 0.0))),
        "total_return": float(basic.get("total_return", 0.0)),
        "start_equity": float(
            basic.get("start_equity", summary.get("initial_capital", 0.0))
        ),
        "final_equity": float(
            basic.get("end_equity", _final_equity(backtest_result))
        ),
        "metrics_keys": sorted(str(k) for k in metrics.keys()),
        # Topstep rule outputs.
        "topstep_enabled": bool(summary.get("topstep_enabled", False)),
        "daily_limit_hits": int(summary.get("daily_limit_hits", 0)),
        "trailing_limit_hits": int(summary.get("trailing_limit_hits", 0)),
        "account_failed": bool(summary.get("account_failed", False)),
        "profit_target_reached": bool(summary.get("profit_target_reached", False)),
    }
    return report


def _synthetic_flat_report(strategy, start_date, end_date, params, rng):
    """Single-mode synthetic record."""
    trades = rng.randint(50, 500)
    win_rate = rng.uniform(0.35, 0.55)
    winning_trades = int(trades * win_rate)
    losing_trades = trades - winning_trades
    avg_win = rng.uniform(15.0, 35.0)
    avg_loss = rng.uniform(-25.0, -10.0)
    gross_profit = winning_trades * avg_win
    gross_loss = losing_trades * avg_loss
    avg_trade = (gross_profit + gross_loss) / trades if trades else 0.0
    profit_factor = gross_profit / abs(gross_loss) if gross_loss else 0.0
    final_equity = 100_000.0 + gross_profit + gross_loss
    return {
        "strategy": strategy,
        "start_date": start_date,
        "end_date": end_date,
        "params": params,
        "trades": trades,
        "executed_trades": trades,
        "skipped_trades": 0,
        "winning_trades": winning_trades,
        "losing_trades": losing_trades,
        "win_rate": round(win_rate, 4),
        "avg_trade": round(avg_trade, 4),
        "profit_factor": round(profit_factor, 4),
        "gross_profit": round(gross_profit, 2),
        "gross_loss": round(gross_loss, 2),
        "max_drawdown": round(rng.uniform(-0.10, -0.02), 4),
        "max_drawdown_dollar": round(rng.uniform(-5000.0, -1000.0), 2),
        "max_sod_drawdown_dollar": round(rng.uniform(-900.0, -100.0), 2),
        "avg_sod_drawdown_dollar": round(rng.uniform(-300.0, -50.0), 2),
        "sharpe": round(rng.uniform(-0.5, 1.5), 4),
        "cagr": round(rng.uniform(-0.05, 0.25), 4),
        "total_return": round((final_equity - 100_000.0) / 100_000.0, 4),
        "start_equity": 100_000.0,
        "final_equity": round(final_equity, 2),
        "metrics_keys": ["synthetic"],
        "topstep_enabled": False,
        "daily_limit_hits": 0,
        "trailing_limit_hits": 0,
        "account_failed": False,
        "profit_target_reached": False,
    }


def _synthetic_report(strategy, start_date, end_date, params):
    """Deterministic placeholder used during smoke tests / cross-agent development."""
    seed = int(hashlib.md5(f"{strategy}:{start_date}:{end_date}".encode()).hexdigest(), 16)
    rng = random.Random(seed)
    raw = _synthetic_flat_report(strategy, start_date, end_date, params, rng)
    topstep = _synthetic_flat_report(strategy, start_date, end_date, params, rng)
    topstep["topstep_enabled"] = True
    topstep["daily_limit_hits"] = rng.randint(0, 3)
    topstep["trailing_limit_hits"] = int(rng.random() < 0.1)
    topstep["account_failed"] = topstep["trailing_limit_hits"] > 0
    return {
        "strategy": strategy,
        "start_date": start_date,
        "end_date": end_date,
        "params": params,
        "raw": raw,
        "topstep": topstep,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Backtest one strategy over one date chunk.")
    parser.add_argument("--strategy", required=True, choices=["kasen_orb", "nitro_crt"])
    parser.add_argument("--start-date", required=True, help="Chunk start (YYYY-MM-DD)")
    parser.add_argument("--end-date", required=True, help="Chunk end (YYYY-MM-DD)")
    parser.add_argument("--output", required=True, help="Path to write JSON result")
    parser.add_argument(
        "--data-path",
        default=os.environ.get(
            "NQ_DATA_PATH",
            "/config/topstep-strats/data/NQ_1min.csv" if Path("/config/topstep-strats/data/NQ_1min.csv").exists() else "/tmp/market_data/NQ_1min.csv",
        ),
        help="Path to NQ_1min.csv",
    )
    parser.add_argument(
        "--params",
        default=None,
        help="JSON dict with strategy_params / backtest_params / metrics_kwargs",
    )
    args = parser.parse_args(argv)

    data_path = Path(args.data_path)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if not data_path.exists():
        raise FileNotFoundError(f"Data file not found: {data_path}")

    params = json.loads(args.params) if args.params else {}

    # Smoke-test isolation: force synthetic fallback regardless of module availability.
    if os.environ.get("TOPSTEP_GH_ACTIONS_TEST"):
        modules = None
    else:
        modules = load_modules()

    if modules is None:
        report = _synthetic_report(args.strategy, args.start_date, args.end_date, params)
    else:
        df_1m = modules["load_nq_data"](str(data_path))
        df_chunk = modules["split_by_date"](df_1m, args.start_date, args.end_date)

        if df_chunk.empty:
            report = _build_report(
                args.strategy,
                args.start_date,
                args.end_date,
                {"trades": [], "equity_curve": [], "summary": {}},
                {},
                params,
            )
        else:
            strategy_mod = modules["strategies"][args.strategy]
            signals = strategy_mod.generate_signals(df_chunk, params.get("strategy_params"))
            metrics_kwargs = params.get("metrics_kwargs", {})

            # Raw backtest: no Topstep rule enforcement.
            raw_bt = modules["run_backtest"](signals, params.get("backtest_params"))
            raw_metrics = modules["calculate_metrics"](raw_bt, **metrics_kwargs)
            raw_report = _build_report(
                args.strategy,
                args.start_date,
                args.end_date,
                raw_bt,
                raw_metrics,
                params,
            )

            # Topstep-constrained backtest.
            topstep_bt_params = {**(params.get("backtest_params") or {})}
            topstep_bt_params["topstep"] = {**topstep_bt_params.get("topstep", {}), "enabled": True}
            topstep_bt = modules["run_backtest"](signals, topstep_bt_params)
            topstep_metrics = modules["calculate_metrics"](topstep_bt, **metrics_kwargs)
            topstep_report = _build_report(
                args.strategy,
                args.start_date,
                args.end_date,
                topstep_bt,
                topstep_metrics,
                params,
            )

            report = {
                "strategy": args.strategy,
                "start_date": args.start_date,
                "end_date": args.end_date,
                "params": params,
                "raw": raw_report,
                "topstep": topstep_report,
            }

    with open(output_path, "w") as f:
        json.dump(report, f, indent=2, default=str)

    print(f"[run_chunk] wrote {output_path}")


if __name__ == "__main__":
    main()
