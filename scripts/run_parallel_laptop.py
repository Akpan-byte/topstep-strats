#!/usr/bin/env python3
"""
run_parallel_laptop.py — Run the full 10-year backtest locally using all
available CPU cores (or a requested number of workers).

Usage:
    python scripts/run_parallel_laptop.py --workers 16 --output-dir laptop_results --instrument NQ --strategy nitro_crt --data-path data/NQ_1min.csv
"""

import argparse
import json
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timedelta
from pathlib import Path


# 20 chunks covering the available 1-minute data (2016-06-01 to 2026-05-29).
CHUNKS = [
    ("2016-06-01", "2016-11-30"),
    ("2016-12-01", "2017-05-31"),
    ("2017-06-01", "2017-11-30"),
    ("2017-12-01", "2018-05-31"),
    ("2018-06-01", "2018-11-30"),
    ("2018-12-01", "2019-05-31"),
    ("2019-06-01", "2019-11-30"),
    ("2019-12-01", "2020-05-31"),
    ("2020-06-01", "2020-11-30"),
    ("2020-12-01", "2021-05-31"),
    ("2021-06-01", "2021-11-30"),
    ("2021-12-01", "2022-05-31"),
    ("2022-06-01", "2022-11-30"),
    ("2022-12-01", "2023-05-31"),
    ("2023-06-01", "2023-11-30"),
    ("2023-12-01", "2024-05-31"),
    ("2024-06-01", "2024-11-30"),
    ("2024-12-01", "2025-05-31"),
    ("2025-06-01", "2025-11-30"),
    ("2025-12-01", "2026-05-29"),
]

STRATEGIES = ["kasen_orb", "nitro_crt"]


def _run_chunk(args):
    """Worker function: run one strategy over one date chunk."""
    project_root, strategy, start, end, instrument, data_path, output_dir = args
    output_file = output_dir / f"result_{strategy}_{instrument}_{start}_{end}.json"
    cmd = [
        sys.executable,
        str(project_root / "scripts" / "run_chunk.py"),
        "--strategy", strategy,
        "--instrument", instrument,
        "--start-date", start,
        "--end-date", end,
        "--output", str(output_file),
        "--data-path", str(data_path),
    ]
    try:
        subprocess.run(cmd, cwd=project_root, check=True, capture_output=True, text=True)
        return (strategy, instrument, start, end, "ok", output_file)
    except subprocess.CalledProcessError as exc:
        return (strategy, instrument, start, end, "failed", exc.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run the full backtest locally in parallel.")
    parser.add_argument("--workers", type=int, default=None, help="Number of parallel workers (default: CPU count)")
    parser.add_argument("--output-dir", default="laptop_results", help="Directory for chunk + aggregate outputs")
    parser.add_argument("--instrument", default="NQ", choices=["NQ", "ES", "YM"], help="Futures instrument")
    parser.add_argument("--strategy", default=None, choices=STRATEGIES, help="Strategy to run (default: both)")
    parser.add_argument("--data-path", default=None, help="Path to 1-minute CSV")
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parent.parent
    output_dir = project_root / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    data_path = Path(args.data_path) if args.data_path else project_root / "data" / f"{args.instrument}_1min.csv"
    if not data_path.exists():
        raise FileNotFoundError(f"Data file not found: {data_path}")

    strategies = [args.strategy] if args.strategy else STRATEGIES

    tasks = [
        (project_root, strategy, start, end, args.instrument, data_path, output_dir)
        for strategy in strategies
        for start, end in CHUNKS
    ]

    workers = args.workers or min(16, len(tasks))
    print(f"[run_parallel_laptop] running {len(tasks)} tasks with {workers} workers")

    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(_run_chunk, task): task for task in tasks}
        for future in as_completed(futures):
            strategy, instrument, start, end, status, payload = future.result()
            if status == "ok":
                print(f"  done: {strategy} {instrument} {start} -> {end}")
            else:
                print(f"  FAILED: {strategy} {instrument} {start} -> {end}\n{payload}", file=sys.stderr)

    aggregate_dir = output_dir / "aggregated"
    subprocess.run(
        [sys.executable, str(project_root / "scripts" / "aggregate.py"),
         "--input", str(output_dir), "--output", str(aggregate_dir)],
        cwd=project_root,
        check=True,
    )
    print(f"[run_parallel_laptop] aggregate report: {aggregate_dir / 'final_report.json'}")


if __name__ == "__main__":
    main()
