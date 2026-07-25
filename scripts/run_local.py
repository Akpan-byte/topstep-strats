# CHANGE_SUMMARY
# 2026-07-25  kilo
#   - Created run_local.py for a quick local smoke test of the full pipeline.
#   - Splits a date range into a few chunks, invokes run_chunk.py for each
#     strategy, then calls aggregate.py on the resulting JSON files.
# WHY: Provides a one-command sanity check before pushing to GitHub Actions.

import argparse
import os
import subprocess
import sys
from datetime import datetime, timedelta
from pathlib import Path


def _split_range(start_str, end_str, n_chunks):
    """Return a list of (start, end) date strings splitting [start, end] evenly."""
    start = datetime.strptime(start_str, "%Y-%m-%d")
    end = datetime.strptime(end_str, "%Y-%m-%d")
    if end <= start:
        raise ValueError("end-date must be after start-date")

    total_days = (end - start).days
    chunk_days = total_days // n_chunks
    chunks = []
    for i in range(n_chunks):
        chunk_start = start + timedelta(days=i * chunk_days)
        chunk_end = (
            start + timedelta(days=(i + 1) * chunk_days)
            if i < n_chunks - 1 else end
        )
        chunks.append((chunk_start.strftime("%Y-%m-%d"), chunk_end.strftime("%Y-%m-%d")))
    return chunks


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run a local smoke test of the parallel pipeline.")
    parser.add_argument("--start-date", default="2024-01-01", help="Start date (YYYY-MM-DD)")
    parser.add_argument("--end-date", default="2024-03-31", help="End date (YYYY-MM-DD)")
    parser.add_argument(
        "--data-path",
        default=os.environ.get(
            "NQ_DATA_PATH",
            "/config/topstep-strats/data/NQ_1min.csv" if Path("/config/topstep-strats/data/NQ_1min.csv").exists() else "/tmp/market_data/NQ_1min.csv",
        ),
        help="Path to NQ_1min.csv",
    )
    parser.add_argument("--output-dir", default="local_results", help="Directory for chunk + aggregate outputs")
    parser.add_argument(
        "--strategies",
        nargs="+",
        default=["kasen_orb", "nitro_crt"],
        choices=["kasen_orb", "nitro_crt"],
        help="Strategies to run",
    )
    parser.add_argument("--n-chunks", type=int, default=2, help="Number of date chunks")
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parent.parent
    output_dir = project_root / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    chunks = _split_range(args.start_date, args.end_date, args.n_chunks)
    run_chunk_script = project_root / "scripts" / "run_chunk.py"

    for strategy in args.strategies:
        for start, end in chunks:
            out_file = output_dir / f"result_{strategy}_{start}_{end}.json"
            cmd = [
                sys.executable,
                str(run_chunk_script),
                "--strategy", strategy,
                "--start-date", start,
                "--end-date", end,
                "--output", str(out_file),
                "--data-path", args.data_path,
            ]
            print("\n[run_local]", " ".join(cmd))
            subprocess.run(cmd, cwd=project_root, check=True)

    aggregate_script = project_root / "scripts" / "aggregate.py"
    agg_out = output_dir / "aggregated"
    agg_cmd = [
        sys.executable,
        str(aggregate_script),
        "--input", str(output_dir),
        "--output", str(agg_out),
    ]
    print("\n[run_local]", " ".join(agg_cmd))
    subprocess.run(agg_cmd, cwd=project_root, check=True)

    print(f"\n[run_local] done. Aggregate report: {agg_out / 'final_report.json'}")


if __name__ == "__main__":
    main()
