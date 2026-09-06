import sys, json, glob
from pathlib import Path
from datetime import datetime

def main():
    input_dir = Path("artifacts") if Path("artifacts").exists() else Path(".")
    files = list(input_dir.glob("**/*results_shard_*.jsonl")) + list(input_dir.glob("results_shard_*.jsonl"))
    print(f"Found {len(files)} shard result files to aggregate.")

    all_records = []
    for f in files:
        with open(f, "r") as fp:
            for line in fp:
                line = line.strip()
                if line:
                    try:
                        all_records.append(json.loads(line))
                    except Exception:
                        pass

    print(f"Total aggregated records across all 20 GitHub runners: {len(all_records):,}")

    out_json = Path("MASTER_10YR_SWEEP_RESULTS.json")
    with open(out_json, "w") as fp:
        json.dump(all_records, fp, indent=2)

    # Leaderboard
    valid = [r for r in all_records if "ALL" in r.get("sessions", {}) and r["sessions"]["ALL"].get("trades", 0) >= 30]
    valid.sort(key=lambda x: -x["sessions"]["ALL"].get("sharpe_ratio", 0.0))

    lines = [
        "# MASTER 10-YEAR MULTI-ASSET QUANT SWEEP LEADERBOARD (GITHUB ACTIONS 20-RUNNER SWARM)",
        f"> **Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S UTC')}  ",
        f"> **Total Parameter Evaluations:** {len(all_records):,}  ",
        f"> **Active Runners:** 20 GitHub Actions Ubuntu Runners  ",
        f"> **Datasets Evaluated:** ES, NQ, YM (10.35M total 1-min bars from June 2016 to May 2026)  ",
        "",
        "| Rank | Strategy | Asset | Net PnL ($) | Win Rate (%) | Trades | Sharpe | Sortino | Max DD (%) | DSR | Best Session | Best Session Sharpe | Key Parameters |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
    ]

    for rank, r in enumerate(valid[:50], 1):
        all_s = r["sessions"]["ALL"]
        best_sess_name = "ALL"
        best_sess_sr = all_s.get("sharpe_ratio", 0.0)
        for s_name, s_data in r["sessions"].items():
            if s_data.get("trades", 0) >= 15 and s_data.get("sharpe_ratio", 0.0) > best_sess_sr:
                best_sess_sr = s_data.get("sharpe_ratio", 0.0)
                best_sess_name = s_name

        p_str = ", ".join(f"{k}:{v}" for k, v in r["params"].items())
        lines.append(
            f"| **{rank}** | `{r['strategy']}` | **{r['symbol']}** | "
            f"**${all_s.get('net_pnl',0.0):,.2f}** | {all_s.get('win_rate',0.0):.1f}% | {all_s.get('trades',0)} | "
            f"**{all_s.get('sharpe_ratio',0.0):.2f}** | {all_s.get('sortino',0.0):.2f} | {all_s.get('max_dd_pct',0.0):.2f}% | "
            f"{all_s.get('dsr',0.0):.2f} | `{best_sess_name}` | **{best_sess_sr:.2f}** | `{p_str}` |"
        )

    out_md = Path("MASTER_10YR_SWEEP_LEADERBOARD.md")
    with open(out_md, "w") as fp:
        fp.write("\n".join(lines) + "\n")

    print(f"Aggregation complete! Wrote {out_json} and {out_md}")

if __name__ == "__main__":
    main()
