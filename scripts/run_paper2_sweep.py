# CHANGE_SUMMARY
# 2026-08-19  coder
#   - Created run_paper2_sweep.py, the Paper-2 matrix orchestrator.
#   - Iterates over a slice of strategy IDs, all requested instruments,
#     sessions (Asian/London/NY), and date chunks, invoking
#     run_paper2_chunk.py for each cell.
#   - Supports the workflow-facing short flags --id-start/--id-end and the
#     long flags --strategy-id-start/--strategy-id-end interchangeably.
#   - Supports a custom single chunk via --start-date/--end-date or an
#     arbitrary list via --chunks; defaults to the 20 six-month CHUNKS.
#   - Supports sequential or parallel execution and an optional final
#     aggregation step via aggregate_paper2.py.
# WHY: Paper-2 is a structured sweep; this script turns the Cartesian product
#      of parameters into concrete chunk artifacts without manual scripting.

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Dict, List, Tuple


_PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from topstep_strats.paper2_registry import (
    CHUNKS,
    INSTRUMENT_CONFIG,
    SESSION_CONFIG,
    default_data_path,
    make_strategy_key,
)


def _resolve_data_path(instrument: str, data_dir: str | None) -> str:
    """Pick the concrete data file for an instrument."""
    if data_dir:
        d = Path(data_dir)
        csv = d / f"{instrument}_1min.csv"
        if csv.exists():
            return str(csv)
        parquet = d / f"{instrument}_1min.parquet"
        if parquet.exists():
            return str(parquet)
        raise FileNotFoundError(f"No data file for {instrument} in {data_dir}")
    return default_data_path(instrument)


def _run_chunk(args: Tuple[Path, int, str, str, str, str, str, Path]) -> Tuple[str, ...]:
    """Worker function passed to ProcessPoolExecutor."""
    project_root, strategy_id, instrument, session, start, end, data_path, output_dir = args
    strategy_key = make_strategy_key(strategy_id, instrument, session)
    output_file = output_dir / f"result_{strategy_key}_{start}_{end}.json"
    cmd = [
        sys.executable,
        str(project_root / "scripts" / "run_paper2_chunk.py"),
        "--strategy-id", str(strategy_id),
        "--instrument", instrument,
        "--session", session,
        "--start-date", start,
        "--end-date", end,
        "--output", str(output_file),
        "--data-path", data_path,
    ]
    try:
        subprocess.run(cmd, cwd=project_root, check=True, capture_output=True, text=True)
        return (strategy_key, start, end, "ok", str(output_file))
    except subprocess.CalledProcessError as exc:
        return (strategy_key, start, end, "failed", exc.stderr or "")


def _build_chunks(args) -> List[Tuple[str, str]]:
    """Resolve the date chunks to run from CLI arguments."""
    if args.chunks:
        chunks = json.loads(args.chunks)
        if not isinstance(chunks, list) or not all(isinstance(c, (list, tuple)) and len(c) == 2 for c in chunks):
            raise ValueError("--chunks must be a JSON list of [start, end] pairs")
        return [(str(c[0]), str(c[1])) for c in chunks]

    if args.start_date and args.end_date:
        return [(args.start_date, args.end_date)]

    return list(CHUNKS)


def _build_tasks(args) -> List[Tuple[Path, int, str, str, str, str, str, Path]]:
    """Build the full Cartesian product of requested sweep dimensions."""
    from topstep_strats.paper2_registry import N_STRATEGIES

    chunks = _build_chunks(args)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    instruments = args.instruments
    sessions = args.sessions
    id_start = max(0, args.strategy_id_start)
    id_end = min(N_STRATEGIES, args.strategy_id_end)
    if id_start >= id_end:
        raise ValueError(
            f"strategy-id-start ({id_start}) must be less than strategy-id-end ({id_end})"
        )

    tasks = []
    for strategy_id in range(id_start, id_end):
        for instrument in instruments:
            data_path = _resolve_data_path(instrument, args.data_dir)
            for session in sessions:
                for start, end in chunks:
                    tasks.append(
                        (_PROJECT_ROOT, strategy_id, instrument, session, start, end, data_path, output_dir)
                    )
    return tasks


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run a slice of the Paper-2 strategy matrix across instruments, sessions, and date chunks."
    )
    parser.add_argument(
        "--strategy-id-start",
        type=int,
        default=None,
        dest="strategy_id_start",
        help="First strategy ID to run (inclusive)",
    )
    parser.add_argument(
        "--id-start",
        type=int,
        default=None,
        dest="strategy_id_start",
        help="Alias for --strategy-id-start",
    )
    parser.add_argument(
        "--strategy-id-end",
        type=int,
        default=None,
        dest="strategy_id_end",
        help="Last strategy ID to run (exclusive)",
    )
    parser.add_argument(
        "--id-end",
        type=int,
        default=None,
        dest="strategy_id_end",
        help="Alias for --strategy-id-end",
    )
    parser.add_argument(
        "--instruments",
        nargs="+",
        default=list(INSTRUMENT_CONFIG.keys()),
        choices=list(INSTRUMENT_CONFIG.keys()),
        help="Instruments to test (default: NQ ES YM)",
    )
    parser.add_argument(
        "--sessions",
        nargs="+",
        default=list(SESSION_CONFIG.keys()),
        choices=list(SESSION_CONFIG.keys()),
        help="Sessions to test (default: Asian London NY)",
    )
    parser.add_argument(
        "--chunks",
        default=None,
        help='JSON list of [start, end] date pairs, e.g. \'[["2024-01-01","2024-01-31"]]\'',
    )
    parser.add_argument(
        "--start-date",
        default=None,
        help="Custom chunk start; ignored if --chunks is provided",
    )
    parser.add_argument(
        "--end-date",
        default=None,
        help="Custom chunk end; ignored if --chunks is provided",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=1,
        help="Parallel workers (default: 1, sequential)",
    )
    parser.add_argument(
        "--output-dir",
        default="paper2_results",
        help="Directory for chunk JSON outputs",
    )
    parser.add_argument(
        "--data-dir",
        default=None,
        help="Directory containing {instrument}_1min.csv/parquet files",
    )
    parser.add_argument(
        "--aggregate",
        action="store_true",
        default=True,
        help="Run aggregate_paper2.py after chunks finish (default: True)",
    )
    parser.add_argument(
        "--no-aggregate",
        action="store_true",
        help="Skip the aggregate step",
    )
    args = parser.parse_args(argv)

    from topstep_strats.paper2_registry import N_STRATEGIES

    if args.strategy_id_start is None:
        args.strategy_id_start = 0
    if args.strategy_id_end is None:
        args.strategy_id_end = N_STRATEGIES

    tasks = _build_tasks(args)
    n_ids = args.strategy_id_end - args.strategy_id_start
    n_insts = len(args.instruments)
    n_sessions = len(args.sessions)
    n_chunks = len(tasks) // max(1, n_ids * n_insts * n_sessions)
    print(
        f"[run_paper2_sweep] {len(tasks)} tasks: strategy-ids "
        f"[{args.strategy_id_start}, {args.strategy_id_end}), instruments={args.instruments}, "
        f"sessions={args.sessions}, chunks={n_chunks}"
    )

    failures = 0
    if args.workers <= 1:
        for task in tasks:
            strategy_key, start, end, status, payload = _run_chunk(task)
            if status == "ok":
                print(f"  ok: {strategy_key} {start} -> {end}")
            else:
                failures += 1
                print(f"  FAILED: {strategy_key} {start} -> {end}\n{payload}", file=sys.stderr)
    else:
        workers = min(args.workers, len(tasks))
        with ProcessPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(_run_chunk, task): task for task in tasks}
            for future in as_completed(futures):
                strategy_key, start, end, status, payload = future.result()
                if status == "ok":
                    print(f"  ok: {strategy_key} {start} -> {end}")
                else:
                    failures += 1
                    print(f"  FAILED: {strategy_key} {start} -> {end}\n{payload}", file=sys.stderr)

    if args.aggregate and not args.no_aggregate:
        aggregate_dir = Path(args.output_dir) / "aggregated"
        aggregate_dir.mkdir(parents=True, exist_ok=True)
        agg_cmd = [
            sys.executable,
            str(_PROJECT_ROOT / "scripts" / "aggregate_paper2.py"),
            "--input", str(args.output_dir),
            "--output", str(aggregate_dir),
        ]
        print(f"[run_paper2_sweep] aggregating: {' '.join(agg_cmd)}")
        subprocess.run(agg_cmd, cwd=_PROJECT_ROOT, check=True)
        print(f"[run_paper2_sweep] aggregate report: {aggregate_dir / 'final_report.json'}")

    print(f"[run_paper2_sweep] done. failures={failures}/{len(tasks)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
