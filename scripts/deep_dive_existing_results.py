#!/usr/bin/env python3
# CHANGE_SUMMARY
# 2026-08-19  kilo
#   - Created deep-dive analysis script for existing Paper-1 backtest results.
#   - Loads topstep_ranked_20260819_084349.csv and casper_paper1_tp_sl_sweep.csv.
#   - Parses instrument/session/blueprint from strategy names for the ranked file.
#   - Ranks viable (alive account, win_rate >= 0.70) rows with source-aware scores:
#       * Casper TP/SL sweep: score = avg_per_week * win_rate
#         (weekly income weighted by hit rate; avg_per_trade used as a tie-breaker)
#       * TopStep ranked file: score = total_return * win_rate
#         (unitless proxy because dollar/trade metrics are not in this file)
#   - Exports all qualifying rows to deep_dive_top_performers.csv and a short
#     summary to deep_dive_summary.md.
# WHY: Provide a reproducible, repeatable deep-dive of current backtest outputs.

from __future__ import annotations

from pathlib import Path

import pandas as pd

PROJECT_ROOT = Path(__file__).resolve().parent.parent

RANKED_CSV = PROJECT_ROOT / "gh_results" / "paper1" / "topstep_ranked_20260819_084349.csv"
CASPER_CSV = PROJECT_ROOT / "gh_results" / "casper_paper1_tp_sl_sweep.csv"
OUT_CSV = PROJECT_ROOT / "gh_results" / "deep_dive_top_performers.csv"
OUT_MD = PROJECT_ROOT / "gh_results" / "deep_dive_summary.md"

WIN_RATE_THRESHOLD = 0.70


def parse_ranked_strategy(name: str):
    """Parse paper1_<id>_<blueprint>_..._<instrument>_<session> names."""
    parts = name.split("_")
    return {
        "blueprint": parts[2],
        "instrument": parts[-2],
        "session": parts[-1],
        "strategy_id": parts[1],
    }


def load_ranked() -> pd.DataFrame:
    df = pd.read_csv(RANKED_CSV)
    df["source"] = "topstep_ranked"
    # account_failed is an integer count in this file; 0 == never failed
    df["alive"] = df["account_failed"] == 0
    parsed = df["strategy"].apply(parse_ranked_strategy).apply(pd.Series)
    df = pd.concat([df, parsed], axis=1)
    df["score_method"] = "total_return * win_rate"
    df["score"] = df["total_return"] * df["win_rate"]
    df["variation"] = df["strategy"]
    for col in [
        "base_id",
        "filter",
        "tp_atr",
        "sl_atr",
        "tp_sl_ratio",
        "avg_per_trade",
        "avg_per_week",
        "total_pnl_dollars",
        "executed_trades",
    ]:
        df[col] = pd.NA
    return df


def load_casper() -> pd.DataFrame:
    df = pd.read_csv(CASPER_CSV)
    df["source"] = "casper_tp_sl_sweep"
    df["alive"] = ~df["account_failed"].astype(bool)
    df["instrument"] = "NQ"
    df["blueprint"] = "casper"
    df["session"] = df["session"].astype(str)
    df["tp_sl_ratio"] = df["tp_atr"] / df["sl_atr"]
    df["score_method"] = "avg_per_week * win_rate"
    df["score"] = df["avg_per_week"] * df["win_rate"]
    df["strategy_id"] = df["base_id"].astype(str)
    df["variation"] = df.apply(
        lambda r: (
            f"paper1_{r['base_id']}_casper_inverted_fvg_NQ_{r['session']}"
            f"_tp{r['tp_atr']}_sl{r['sl_atr']}_{r['filter']}"
        ),
        axis=1,
    )
    df["total_trades"] = df["trades"]
    df["profit_target_reached"] = pd.NA
    df["sharpe"] = pd.NA
    df["profit_factor"] = pd.NA
    return df


def select_columns(df: pd.DataFrame) -> pd.DataFrame:
    cols = [
        "source",
        "variation",
        "strategy_id",
        "blueprint",
        "instrument",
        "session",
        "win_rate",
        "total_return",
        "avg_per_trade",
        "avg_per_week",
        "total_pnl_dollars",
        "executed_trades",
        "total_trades",
        "account_failed",
        "profit_target_reached",
        "filter",
        "tp_atr",
        "sl_atr",
        "tp_sl_ratio",
        "score",
        "score_method",
    ]
    present = [c for c in cols if c in df.columns]
    return df[present].copy()


def breakdown_table(df: pd.DataFrame, keys: list[str]) -> pd.DataFrame:
    cols = [k for k in keys if k in df.columns]
    if not cols:
        return pd.DataFrame()
    return (
        df.groupby(cols, dropna=False)
        .agg(count=("score", "size"), median_score=("score", "median"))
        .reset_index()
        .sort_values("median_score", ascending=False)
    )


def main() -> None:
    ranked = load_ranked()
    casper = load_casper()

    qualifying_ranked = ranked[
        ranked["alive"] & (ranked["win_rate"] >= WIN_RATE_THRESHOLD)
    ].copy()
    qualifying_casper = casper[
        casper["alive"] & (casper["win_rate"] >= WIN_RATE_THRESHOLD)
    ].copy()

    combined = pd.concat(
        [select_columns(qualifying_ranked), select_columns(qualifying_casper)],
        ignore_index=True,
    )
    combined.to_csv(OUT_CSV, index=False)

    # ---- Casper-specific thresholds (all rows, including blown accounts) ----
    high_per_trade_all = casper[
        (casper["avg_per_trade"] >= 75) & (casper["win_rate"] >= 0.80)
    ].copy()
    high_per_trade_alive = high_per_trade_all[high_per_trade_all["alive"]].copy()

    high_weekly_all = casper[
        (casper["avg_per_week"] >= 300) & (casper["win_rate"] >= 0.80)
    ].copy()
    high_weekly_alive = high_weekly_all[high_weekly_all["alive"]].copy()

    # ---- Top tables ----
    top_casper = qualifying_casper.sort_values("score", ascending=False).head(15)
    top_ranked = qualifying_ranked.sort_values("score", ascending=False).head(15)
    best_per_pair = (
        qualifying_ranked.loc[
            qualifying_ranked.groupby(["instrument", "session"])["score"].idxmax()
        ]
        .sort_values(["instrument", "session"])
        .copy()
    )

    # ---- Markdown summary ----
    md = []
    md.append("# Deep-Dive Summary: Existing Backtest Results")
    md.append("")
    md.append(f"- Generated: {pd.Timestamp.now(tz='UTC').strftime('%Y-%m-%d %H:%M UTC')}")
    md.append(f"- Win-rate threshold: {WIN_RATE_THRESHOLD:.0%}")
    md.append(f"- Qualifying rows written to: `{OUT_CSV.relative_to(PROJECT_ROOT)}`")
    md.append("")
    md.append("## Score formulas")
    md.append("")
    md.append("- **Casper TP/SL sweep:** `score = avg_per_week * win_rate`.")
    md.append("  This weights weekly income by consistency. `avg_per_trade` is used as a tie-breaker and for threshold checks.")
    md.append("- **TopStep ranked file:** `score = total_return * win_rate`.")
    md.append("  This is a unitless proxy because the ranked file does not contain dollar/trade metrics.")
    md.append("")
    md.append("## Row counts")
    md.append("")
    md.append(f"- TopStep ranked qualifying rows (alive & win_rate >= {WIN_RATE_THRESHOLD:.0%}): **{len(qualifying_ranked)}**")
    md.append(f"- Casper TP/SL qualifying rows (alive & win_rate >= {WIN_RATE_THRESHOLD:.0%}): **{len(qualifying_casper)}**")
    md.append(f"- Combined qualifying rows in `{OUT_CSV.name}`: **{len(combined)}**")
    md.append("")
    md.append("## Casper TP/SL: top 15 by score")
    md.append("")
    md.append(top_casper[["variation", "win_rate", "avg_per_trade", "avg_per_week", "tp_atr", "sl_atr", "tp_sl_ratio", "score"]].to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("## Casper TP/SL: high avg_per_trade (>= $75) and win_rate (>= 80%)")
    md.append("")
    md.append(f"- Variations meeting thresholds (any account state): **{len(high_per_trade_all)}**")
    md.append(f"- Of those, with alive account: **{len(high_per_trade_alive)}**")
    if not high_per_trade_all.empty:
        md.append("- Top rows by avg_per_trade:")
        md.append("")
        md.append(high_per_trade_all.sort_values("avg_per_trade", ascending=False).head(10)[["variation", "win_rate", "avg_per_trade", "avg_per_week", "account_failed"]].to_markdown(index=False, floatfmt=".4f"))
    else:
        md.append("- No rows meet both thresholds.")
    md.append("")
    md.append("## Casper TP/SL: high avg_per_week (>= $300) and win_rate (>= 80%)")
    md.append("")
    md.append(f"- Variations meeting thresholds (any account state): **{len(high_weekly_all)}**")
    md.append(f"- Of those, with alive account: **{len(high_weekly_alive)}**")
    md.append("")
    md.append("## TopStep ranked: best variation per (instrument, session) pair")
    md.append("")
    md.append(best_per_pair[["instrument", "session", "variation", "blueprint", "win_rate", "total_return", "score"]].to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("## Breakdowns of top performers")
    md.append("")
    md.append("### Casper TP/SL by session (top 15)")
    md.append("")
    md.append(breakdown_table(top_casper, ["session"]).to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("### Casper TP/SL by filter (top 15)")
    md.append("")
    md.append(breakdown_table(top_casper, ["filter"]).to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("### Casper TP/SL by TP/SL ratio bucket (top 15)")
    top_casper_b = top_casper.copy()
    top_casper_b["tp_sl_bucket"] = pd.cut(
        top_casper_b["tp_sl_ratio"],
        bins=[0, 0.05, 0.10, 0.20, 1.0],
        labels=["<=0.05", "0.05-0.10", "0.10-0.20", ">0.20"],
    )
    md.append("")
    md.append(breakdown_table(top_casper_b, ["tp_sl_bucket"]).to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("### TopStep ranked by instrument / session (all qualifying)")
    md.append("")
    md.append(breakdown_table(qualifying_ranked, ["instrument", "session"]).to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("### TopStep ranked by blueprint (all qualifying)")
    md.append("")
    md.append(breakdown_table(qualifying_ranked, ["blueprint"]).to_markdown(index=False, floatfmt=".4f"))
    md.append("")
    md.append("### Casper TP/SL by blueprint (all qualifying)")
    md.append("")
    md.append(breakdown_table(qualifying_casper, ["blueprint"]).to_markdown(index=False, floatfmt=".4f"))
    md.append("")

    OUT_MD.write_text("\n".join(md))

    # ---- Console output ----
    print("\n=== CASPER TP/SL SWEEP: TOP 15 BY SCORE (avg_per_week * win_rate) ===")
    print(
        top_casper[
            ["variation", "win_rate", "avg_per_trade", "avg_per_week", "tp_atr", "sl_atr", "tp_sl_ratio", "score"]
        ].to_string(index=False, float_format=lambda x: f"{x:.4f}")
    )

    print("\n=== TOPSTEP RANKED: TOP 15 BY SCORE (total_return * win_rate) ===")
    print(
        top_ranked[
            ["variation", "blueprint", "instrument", "session", "win_rate", "total_return", "score"]
        ].to_string(index=False, float_format=lambda x: f"{x:.4f}")
    )

    print("\n=== BEST VARIATION PER (INSTRUMENT, SESSION) PAIR (TopStep ranked) ===")
    print(
        best_per_pair[
            ["instrument", "session", "variation", "blueprint", "win_rate", "total_return", "score"]
        ].to_string(index=False, float_format=lambda x: f"{x:.4f}")
    )

    print(f"\nWrote {len(combined)} qualifying rows to {OUT_CSV}")
    print(f"Wrote summary to {OUT_MD}")


if __name__ == "__main__":
    main()
