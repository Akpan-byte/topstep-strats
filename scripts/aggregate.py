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
#   - Fixed equity reconstruction: chained point PnL (gross_profit/gross_loss,
#     which are in points) is applied to a single $100k base across all chunks
#     in date order, producing a true equity_curve, total_return, and CAGR.
#     The old code summed per-chunk dollar equity deltas and took a
#     trade-weighted average of per-chunk CAGRs, which was meaningless.
#   - "overall" now uses only the reentries scenario (the superset config)
#     instead of merging both scenarios, which double-counted first_only trades.
#   - Chart now plots chained equity per scenario instead of cumulative avg_trade.
# 2026-08-03  coder
#   - Added per-instrument grouping (NQ/ES/YM) using the reentries-preference
#     rule so instruments (different point values) are never mixed in one equity
#     reconstruction, and dollar conversion uses each row's own point value.
# 2026-08-03  coder
#   - Added by_config grouping (instrument x htf_timeframe x target_mode x
#     scenario) and a sorted sweep_summary table so the HTF/target sweep report
#     shows every config's 10-year totals ranked by net points.
# WHY: Each chunk now outputs two backtests (unconstrained + Topstep rules),
#      so the aggregate must combine both, and the scenario split is needed to
#      compare one-entry-per-day vs re-entry results. Correct equity math is
#      required to report a credible CAGR / total return.

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
    """Aggregate a set of chunk records into combined summary statistics.

    Equity is reconstructed from *point* PnL (gross_profit/gross_loss, which
    run_chunk.py stores in points) on a single shared capital base. Summing the
    per-chunk dollar equity deltas would be misleading because each chunk
    independently starts from ``initial_capital``; chaining points on one base
    gives a true compounded equity curve, total return, and CAGR.
    """
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
            "total_pnl_dollars": 0.0,
            "start_equity": 0.0,
            "final_equity": 0.0,
            "equity_curve": [],
            "daily_limit_hits": 0,
            "trailing_limit_hits": 0,
            "account_failed_count": 0,
            "profit_target_reached_count": 0,
        }

    df = df.sort_values("start_date").reset_index(drop=True)
    total_trades = int(df["trades"].sum())
    gross_profit = float(df["gross_profit"].sum())
    gross_loss = float(df["gross_loss"].sum())
    net_points = gross_profit + gross_loss

    # Dollar conversion is per-row because a group may mix instruments with
    # different point values (NQ $20, ES $50, YM $5).
    pv = df["point_value"].fillna(20.0).astype(float)
    start_equity = float(df["start_equity"].iloc[0]) if "start_equity" in df.columns and df["start_equity"].notna().any() else 100_000.0
    total_pnl_dollars = float(((df["gross_profit"] + df["gross_loss"]) * pv).sum())

    # Chained equity curve: cumulative point PnL applied to one capital base.
    equity_curve = []
    running = start_equity
    for _, row in df.iterrows():
        running += float(row["gross_profit"] + row["gross_loss"]) * float(row["point_value"])
        equity_curve.append({"date": str(row["end_date"]), "equity": running})
    final_equity = running

    total_return = (final_equity - start_equity) / start_equity if start_equity else 0.0
    try:
        first_ts = pd.Timestamp(df["start_date"].iloc[0])
        last_ts = pd.Timestamp(df["end_date"].iloc[-1])
        years = (last_ts - first_ts).days / 365.25
    except Exception:
        years = 0.0
    cagr = (final_equity / start_equity) ** (1.0 / years) - 1.0 if years > 0 and start_equity > 0 else 0.0

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
        "cagr": cagr,
        "total_return": total_return,
        "total_pnl_points": net_points,
        "total_pnl_dollars": total_pnl_dollars,
        "start_equity": start_equity,
        "final_equity": final_equity,
        "equity_curve": equity_curve,
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

    # Plot chained equity per instrument + scenario on a shared $100k base.
    # Point values differ per instrument, so use each row's own $/pt.
    start_equity = 100_000.0
    if "start_equity" in df.columns and df["start_equity"].notna().any():
        start_equity = float(df["start_equity"].iloc[0])
    styles = {}
    for (instrument, scenario), group in df.groupby(["instrument", "scenario"]):
        group = group.sort_values("start_date").copy()
        pv = group["point_value"].fillna(20.0)
        equity = start_equity + ((group["gross_profit"] + group["gross_loss"]) * pv).cumsum()
        color = {"NQ": "#1f77b4", "ES": "#ff7f0e", "YM": "#2ca02c"}.get(str(instrument), None)
        ax.plot(group["end_date"], equity, marker="o", label=f"{instrument} {scenario}", color=color)

    ax.set_title(f"Chained Equity, $100k Base ({label})")
    ax.set_xlabel("Chunk End Date")
    ax.set_ylabel("Equity ($)")
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
        mode_rec["instrument"] = rec.get("instrument", rec.get("params", {}).get("instrument", "NQ"))
        mode_rec["htf_timeframe"] = rec.get("htf_timeframe", "5m")
        mode_rec["target_mode"] = rec.get("target_mode", "fixed_rr")
        mode_rec["start_date"] = rec["start_date"]
        mode_rec["end_date"] = rec["end_date"]
        # Point value per chunk so equity reconstruction uses the right $/pt.
        try:
            mode_rec["point_value"] = float(rec["params"]["backtest_params"]["point_value"])
        except (KeyError, TypeError, ValueError):
            mode_rec["point_value"] = 20.0
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

    # "overall" must be a single coherent configuration. Reentries is a
    # superset of first_only (it contains the first setup plus all re-entries),
    # so merging both scenarios would double-count every first_only trade.
    scenarios = sorted(df["scenario"].unique().tolist())
    overall_df = df[df["scenario"] == "reentries"] if "reentries" in scenarios else df

    def _prefer_reentries(sub):
        sub_scenarios = sorted(sub["scenario"].unique().tolist())
        if "reentries" in sub_scenarios:
            return sub[sub["scenario"] == "reentries"]
        return sub

    # Full sweep grouping: every (instrument, htf, target, scenario) combo gets
    # its own 10-year aggregate. Key format is machine-parseable for the report.
    by_config = {
        f"{instrument}__{htf}__{target}__{scenario}": _aggregate_group(group)
        for (instrument, htf, target, scenario), group in df.groupby(
            ["instrument", "htf_timeframe", "target_mode", "scenario"], sort=True
        )
    }

    # Ranked sweep summary: each config's headline stats, best net points first.
    def _sweep_summary():
        rows = []
        for key, agg in by_config.items():
            inst, htf, target, scenario = key.split("__")
            rows.append({
                "config": key,
                "instrument": inst,
                "htf_timeframe": htf,
                "target_mode": target,
                "scenario": scenario,
                "trades": agg["total_trades"],
                "win_rate": agg["win_rate"],
                "profit_factor": agg["profit_factor"],
                "net_points": agg["total_pnl_points"],
                "net_dollars": agg["total_pnl_dollars"],
                "cagr": agg["cagr"],
                "max_drawdown_dollar": agg["max_drawdown_dollar"],
            })
        return sorted(rows, key=lambda r: r["net_points"], reverse=True)

    return {
        "overall": _aggregate_group(overall_df),
        "overall_scenario": scenarios,
        "by_scenario": {
            scenario: _aggregate_group(group)
            for scenario, group in df.groupby("scenario", sort=True)
        },
        "by_instrument": {
            instrument: _aggregate_group(_prefer_reentries(group))
            for instrument, group in df.groupby("instrument", sort=True)
        },
        "by_instrument_scenario": {
            f"{instrument}__{scenario}": _aggregate_group(group)
            for (instrument, scenario), group in df.groupby(["instrument", "scenario"], sort=True)
        },
        "by_config": by_config,
        "sweep_summary": _sweep_summary(),
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
