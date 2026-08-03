# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created aggregate.py to combine per-chunk JSON artifacts into a final
#     report and a simple equity-style chart.
# 2026-07-25  kilo
#   - Updated aggregation to handle both "raw" and "topstep" nested reports
#     produced by run_chunk.py. Overall and per-strategy summaries are now
#     emitted for both modes.
# 2026-08-03  coder
#   - Added per-scenario grouping (first_only vs reentries) alongside the
#     existing per-strategy grouping.
# WHY: Each chunk now outputs two backtests (unconstrained + Topstep rules),
#      so the aggregate must combine both, and the scenario split is needed to
#      compare one-entry-per-day vs re-entry results.

import argparse
import json
import sys
from pathlib import Path

import pandas as pd


NUMERIC_COLS = [
    "trades", "executed_trades", "skipped_trades", "winning_trades",
    "losing_trades", "win_rate", "avg_trade", "profit_factor",
    "gross_profit", "gross_loss", "max_drawdown", "max_drawdown_dollar",
    "max_sod_drawdown_dollar", "avg_sod_drawdown_dollar", "sharpe", "cagr",
    "total_return", "start_equity", "final_equity",
    "daily_limit_hits", "trailing_limit_hits",
]

METRICS_SCALAR_COLS = [
    "psr", "dsr",
    "markov_strength", "markov_chi2", "markov_pvalue",
    "brownian_vr", "brownian_z", "brownian_pvalue",
    "bayesian_sharpe_mean", "bayesian_sharpe_median",
    "linear_r2", "linear_slope",
    "exponential_r2", "exponential_growth_rate",
    "quadratic_r2", "polynomial_r2",
    "probability_of_ruin",
    "mc_terminal_wealth_mean", "mc_sharpe_mean", "mc_cagr_mean", "mc_max_drawdown_mean",
    "boot_sharpe_mean", "boot_cagr_mean", "boot_max_drawdown_mean", "boot_win_rate_mean",
]

METRICS_CI_COLS = [
    "bayesian_sharpe_ci95",
    "mc_terminal_wealth_ci95", "mc_sharpe_ci95", "mc_cagr_ci95", "mc_max_drawdown_ci95",
    "boot_sharpe_ci95", "boot_cagr_ci95", "boot_max_drawdown_ci95", "boot_win_rate_ci95",
]


def _weighted_average(df, value_col, weight_col):
    """Weighted average of `value_col` using `weight_col`; safe for empty groups."""
    total = df[weight_col].sum()
    if total == 0:
        return 0.0
    return float((df[value_col] * df[weight_col]).sum() / total)


def _aggregate_group(df):
    """Aggregate a set of chunk records into combined summary statistics."""
    if df.empty:
        return {
            "chunks": 0,
            "total_trades": 0,
            "executed_trades": 0,
            "skipped_trades": 0,
            "winning_trades": 0,
            "losing_trades": 0,
            "win_rate": 0.0,
            "avg_trade": 0.0,
            "profit_factor": 0.0,
            "gross_profit": 0.0,
            "gross_loss": 0.0,
            "max_drawdown": 0.0,
            "max_drawdown_dollar": 0.0,
            "max_sod_drawdown_dollar": 0.0,
            "avg_sod_drawdown_dollar": 0.0,
            "sharpe": 0.0,
            "cagr": 0.0,
            "total_return": 0.0,
            "total_pnl_points": 0.0,
            "start_equity": 0.0,
            "final_equity": 0.0,
            "daily_limit_hits": 0,
            "trailing_limit_hits": 0,
            "account_failed_count": 0,
            "profit_target_reached_count": 0,
        }

    total_trades = int(df["trades"].sum())
    gross_profit = float(df["gross_profit"].sum())
    gross_loss = float(df["gross_loss"].sum())
    start_equity = float(df["start_equity"].mean()) if df["start_equity"].notna().any() else 100_000.0
    total_pnl = float((df["final_equity"] - df["start_equity"]).sum())

    metrics_summary = _aggregate_metrics_summary(df)

    return {
        "chunks": int(len(df)),
        "total_trades": total_trades,
        "executed_trades": int(df["executed_trades"].sum()),
        "skipped_trades": int(df["skipped_trades"].sum()),
        "winning_trades": int(df["winning_trades"].sum()),
        "losing_trades": int(df["losing_trades"].sum()),
        "win_rate": _weighted_average(df, "win_rate", "trades"),
        "avg_trade": _weighted_average(df, "avg_trade", "trades"),
        "profit_factor": gross_profit / abs(gross_loss) if gross_loss else 0.0,
        "gross_profit": gross_profit,
        "gross_loss": gross_loss,
        "max_drawdown": float(df["max_drawdown"].min()),
        "max_drawdown_dollar": float(df["max_drawdown_dollar"].min()),
        "max_sod_drawdown_dollar": float(df["max_sod_drawdown_dollar"].max()),
        "avg_sod_drawdown_dollar": float(df["avg_sod_drawdown_dollar"].mean()),
        "sharpe": _weighted_average(df, "sharpe", "trades"),
        "cagr": _weighted_average(df, "cagr", "trades"),
        "total_return": total_pnl / start_equity if start_equity else 0.0,
        "total_pnl_points": total_pnl,
        "start_equity": start_equity,
        "final_equity": start_equity + total_pnl,
        "daily_limit_hits": int(df["daily_limit_hits"].sum()),
        "trailing_limit_hits": int(df["trailing_limit_hits"].sum()),
        "account_failed_count": int(df["account_failed"].sum()),
        "profit_target_reached_count": int(df["profit_target_reached"].sum()),
        "metrics_summary": metrics_summary,
    }


def _aggregate_metrics_summary(df):
    """Average scalar quant-suite metrics across chunks.

    Scalar metrics are weighted by trade count; confidence intervals are
    averaged component-wise (lower bound and upper bound separately).
    """
    result = {}
    for col in METRICS_SCALAR_COLS:
        if col not in df.columns:
            result[col] = 0.0
            continue
        s = pd.to_numeric(df[col], errors="coerce").fillna(0.0)
        result[col] = _weighted_average(df.assign(__m=s), "__m", "trades")

    for col in METRICS_CI_COLS:
        if col not in df.columns:
            result[col] = [0.0, 0.0]
            continue
        # Each cell is a [low, high] list; build arrays safely.
        lows, highs = [], []
        for val in df[col]:
            if isinstance(val, (list, tuple)) and len(val) >= 2:
                lows.append(float(val[0]))
                highs.append(float(val[1]))
            else:
                lows.append(0.0)
                highs.append(0.0)
        result[col] = [
            _weighted_average(df.assign(__m=lows), "__m", "trades"),
            _weighted_average(df.assign(__m=highs), "__m", "trades"),
        ]
    return result


def _load_records(input_dir):
    """Read every JSON file produced by run_chunk.py."""
    records = []
    for path in sorted(Path(input_dir).rglob("*.json")):
        try:
            with open(path, "r") as f:
                rec = json.load(f)
            if isinstance(rec, dict) and "strategy" in rec and "raw" in rec and "topstep" in rec:
                records.append(rec)
        except Exception as exc:  # pragma: no cover - tolerate corrupt artifacts
            print(f"[aggregate] skipping {path}: {exc}", file=sys.stderr)
    return records


def _make_chart(df, output_dir, label):
    """Plot cumulative average-trade by chunk start date for each strategy.

    Charting is best-effort: if matplotlib is unavailable the chart is skipped
    so aggregation can still run on lean environments (e.g. Akpan laptop).
    """
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as exc:  # pragma: no cover - optional dependency
        print(f"[aggregate] matplotlib unavailable, skipping chart: {exc}", file=sys.stderr)
        return None

    chart_path = Path(output_dir) / f"equity_by_strategy_{label}.png"
    fig, ax = plt.subplots(figsize=(10, 6))

    for strategy, group in df.groupby("strategy", sort=True):
        group = group.sort_values("start_date").copy()
        group["cum_avg_trade"] = group["avg_trade"].cumsum()
        ax.plot(group["start_date"], group["cum_avg_trade"], marker="o", label=str(strategy))

    ax.set_title(f"Cumulative Average Trade by Strategy Chunk ({label})")
    ax.set_xlabel("Chunk Start Date")
    ax.set_ylabel("Cumulative Avg Trade (NQ points)")
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.xticks(rotation=45, ha="right")
    plt.tight_layout()
    plt.savefig(chart_path)
    plt.close(fig)
    return str(chart_path)


def _build_report_for_mode(records, mode, out_dir):
    """Build the aggregate report for one mode ('raw' or 'topstep')."""
    rows = []
    for rec in records:
        mode_rec = rec.get(mode, {}).copy()
        mode_rec["strategy"] = rec.get("strategy", "nitro_crt")
        mode_rec["scenario"] = rec.get("scenario", "reentries")
        mode_rec["start_date"] = rec["start_date"]
        mode_rec["end_date"] = rec["end_date"]
        rows.append(mode_rec)

    df = pd.DataFrame(rows)

    # Flatten nested metrics_summary dicts into top-level columns.
    metrics_rows = []
    for rec in records:
        metrics_rows.append(rec.get(mode, {}).get("metrics_summary", {}))
    if any(metrics_rows):
        metrics_df = pd.DataFrame(metrics_rows)
        for col in metrics_df.columns:
            df[col] = metrics_df[col].values

    for col in NUMERIC_COLS:
        if col not in df.columns:
            df[col] = 0.0
        else:
            df[col] = pd.to_numeric(df[col], errors="coerce").fillna(0.0)

    for col in ["account_failed", "profit_target_reached"]:
        if col not in df.columns:
            df[col] = False
        else:
            df[col] = df[col].astype(bool)

    return {
        "overall": _aggregate_group(df),
        "by_scenario": {
            scenario: _aggregate_group(group)
            for scenario, group in df.groupby("scenario", sort=True)
        },
        "by_strategy": {
            strategy: _aggregate_group(group)
            for strategy, group in df.groupby("strategy", sort=True)
        },
        "chart_files": [_make_chart(df, out_dir, mode)],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Aggregate chunk JSONs into a final report.")
    parser.add_argument("--input", required=True, help="Directory containing chunk JSON files")
    parser.add_argument("--output", required=True, help="Directory to write final_report.json and chart")
    args = parser.parse_args(argv)

    input_dir = Path(args.input)
    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    records = _load_records(input_dir)
    if not records:
        raise RuntimeError(f"No valid chunk JSON files found in {input_dir}")

    report = {
        "raw": _build_report_for_mode(records, "raw", output_dir),
        "topstep": _build_report_for_mode(records, "topstep", output_dir),
        "chunks": records,
    }

    report_path = output_dir / "final_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, default=str)

    print(f"[aggregate] wrote {report_path}")


if __name__ == "__main__":
    main()
