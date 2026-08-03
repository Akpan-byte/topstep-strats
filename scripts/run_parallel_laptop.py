#!/usr/bin/env python3
"""
run_parallel_laptop.py — Run the YM slice of the HTF/target sweep on the Akpan
laptop using all available CPU cores, then package the results so the GitHub
Actions aggregate job can merge them with the NQ/ES sweep.

Usage:
    python scripts/run_parallel_laptop.py --workers 16 \
        --output-dir laptop_results --data-path /tmp/mktdata/YM_1min.parquet
"""

import argparse
import json
import subprocess
import sys
import tarfile
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_sweep_job import CHUNKS, HTFS, TARGET_MODES, SCENARIOS, run_config  # noqa: E402

INSTRUMENTS = ["YM"]


def _run_config_worker(args):
    """Worker: run one sweep config over all chunks, return a summary line."""
    instrument, htf, target_mode, scenario, data_path, output_dir, warmup_days = args
    ok, elapsed = run_config(instrument, htf, target_mode, scenario, data_path, output_dir, warmup_days)
    return f"{instrument}/{htf}/{target_mode}/{scenario}: {ok} chunks in {elapsed:.1f}s"


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run the YM sweep slice locally in parallel.")
    parser.add_argument("--workers", type=int, default=None, help="Parallel workers (default: CPU count)")
    parser.add_argument("--output-dir", default="laptop_results", help="Directory for config chunk JSONs")
    parser.add_argument("--data-path", default=None, help="Path to YM_1min.parquet (or CSV)")
    parser.add_argument("--warmup-days", type=int, default=7)
    parser.add_argument("--upload", action="store_true",
                        help="Tar results and upload to the data-v2.0 release for GH aggregate to merge")
    args = parser.parse_args(argv)

    project_root = Path(__file__).resolve().parent.parent
    output_dir = project_root / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    data_path = Path(args.data_path) if args.data_path else project_root / "data" / "YM_1min.csv"
    if not data_path.exists():
        raise FileNotFoundError(f"Data file not found: {data_path}")

    tasks = [
        (instrument, htf, target_mode, scenario, str(data_path), output_dir, args.warmup_days)
        for instrument in INSTRUMENTS
        for htf in HTFS
        for target_mode in TARGET_MODES
        for scenario in SCENARIOS
    ]
    total = len(tasks)
    workers = args.workers or min(16, total)
    print(f"[run_parallel_laptop] running {total} configs ({len(CHUNKS)} chunks each) with {workers} workers")

    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(_run_config_worker, task): task for task in tasks}
        done = 0
        for future in as_completed(futures):
            try:
                print(f"  done: {future.result()}")
            except Exception as exc:  # pragma: no cover - surface worker failures
                print(f"  FAILED: {futures[future]} -> {exc}", file=sys.stderr)
            done += 1
            print(f"  [{done}/{total}]")

    if args.upload:
        tarball = project_root / "laptop_results.tar.gz"
        with tarfile.open(tarball, "w:gz") as tf:
            tf.add(output_dir, arcname=".")
        subprocess.run(
            ["gh", "release", "upload", "data-v2.0", "--repo", "Akpan-byte/topstep-strats",
             str(tarball), "--clobber"],
            cwd=project_root, check=True,
        )
        print(f"[run_parallel_laptop] uploaded {tarball} — GH aggregate will merge it")

    print(f"[run_parallel_laptop] done. Results in {output_dir}")


if __name__ == "__main__":
    main()
