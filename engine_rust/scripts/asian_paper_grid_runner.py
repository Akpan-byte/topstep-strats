# CHANGE_SUMMARY
# 2026-08-22  kilo
#   - Created engine_rust/scripts/asian_paper_grid_runner.py to run a full
#     parameter sweep over the 108-row Asian-paper matrix.
#   - Sweeps tp_atr x sl_atr x mode x account_size per strategy ID.
#   - Splits the 108 strategy IDs across GitHub Actions matrix workers via
#     --id-start/--id-end and writes one CSV per worker.
#   - Signals are generated once per strategy ID and reused across the grid
#     to avoid redundant indicator computation.
# WHY: sweep_engine.py --paper asian_paper runs a single TP/SL/mode per
#      strategy ID from the matrix.  The paper workflow needs an explicit grid
#      search over exit and account-size combinations.

"""Grid runner for the Asian-paper 108-strategy matrix.

Usage:
    python engine_rust/scripts/asian_paper_grid_runner.py \
        --id-start 0 --id-end 6 \
        --workers 4 \
        --output-dir ./asian_paper_grid_results

The script assumes it is invoked from the repo root and that the Rust engine
has already been built/installed (e.g. `cd engine_rust && maturin develop`).
"""

from __future__ import annotations

import argparse
import itertools
import multiprocessing as mp
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Dict, List, Tuple

import pandas as pd

# Ensure repo root is on sys.path when invoked from engine_rust/scripts/.
_PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))

from engine_rust.scripts.portfolio_runner import apply_runner_to_signals
from engine_rust.scripts.sweep_engine import (
    OUTPUT_COLUMNS_ASIAN_PAPER,
    POINT_VALUES,
    TICK_SIZES,
    _attach_tp_sl,
    _build_asian_paper_cfg,
    _load_instrument_df,
    _run_rust_backtest,
    _runner_kwargs,
    _worker_init,
)
from topstep_strats.data import get_session_mask, split_by_date
from topstep_strats.strategies.asian_paper_matrix import (
    generate_signals as generate_asian_paper_signals,
    list_strategy_ids as list_asian_paper_ids,
)


# ---------------------------------------------------------------------------
# Parameter grid
# ---------------------------------------------------------------------------
TP_ATR_GRID = [0.05, 0.1, 0.2, 0.3, 0.5, 0.75]
SL_ATR_GRID = [1.0, 1.5, 2.0, 2.5, 3.0]
MODE_GRID = ["baseline", "trail", "breakeven_then_trail", "hold_session"]
ACCOUNT_SIZE_GRID = ["50k", "150k"]

def _empty_record(
    sid: str,
    cfg: Dict[str, Any],
    tp: float,
    sl: float,
    mode: str,
    account_size: str,
    topstep_enabled: bool,
) -> Dict[str, Any]:
    """Return a zero-filled record when no signals/trades are produced."""
    return {
        "paper": "asian_paper",
        "strategy_id": sid,
        "engine": cfg.get("engine", ""),
        "instrument": cfg.get("instrument", ""),
        "session": cfg.get("session", ""),
        "target_style": cfg.get("target_style", ""),
        "tp_atr": tp,
        "sl_atr": sl,
        "mode": mode,
        "win_rate": 0.0,
        "n_trades": 0,
        "executed_trades": 0,
        "trades_per_day": 0.0,
        "account_blowups": 0,
        "daily_limit_hits": 0,
        "trailing_limit_hits": 0,
        "max_drawdown": 0.0,
        "sharpe": 0.0,
        "total_return": 0.0,
        "total_pnl_dollars": 0.0,
        "avg_per_trade": 0.0,
        "avg_per_week": 0.0,
        "avg_per_day": 0.0,
        "profit_target_reached": False,
        "account_failed": False,
        "passed_combines": 0,
        "failed_combines": 0,
        "combine_pass_rate": 0.0,
        "avg_days_to_pass": 0.0,
        "median_days_to_pass": 0.0,
        "min_days_to_pass": 0,
        "max_days_to_pass": 0,
        "days_to_pass_p50": 0.0,
        "days_to_pass_p75": 0.0,
        "days_to_pass_p90": 0.0,
        "days_to_pass_p95": 0.0,
        "days_to_pass_p99": 0.0,
        "avg_trades_to_pass": 0.0,
        "median_trades_to_pass": 0.0,
        "topstep_enabled": topstep_enabled,
        "account_size": account_size,
    }


def _run_asian_paper_id(
    args: Tuple[str, str, str, str, bool],
) -> List[Dict[str, Any]]:
    """Run the full grid for a single strategy ID.

    Signals are generated once per ID and reused across all TP/SL/mode/account
    combinations to avoid redundant indicator computation.
    """
    sid, start, end, data_dir, topstep_enabled = args

    cfg = _build_asian_paper_cfg(sid, start, end)
    instrument = cfg["instrument"]
    df = _load_instrument_df(instrument)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start, end)

    try:
        entry_signals = generate_asian_paper_signals(df_s, cfg)
    except Exception as exc:
        print(f"  Asian-paper signal generation failed for {sid}: {exc}", file=sys.stderr)
        return [
            _empty_record(sid, cfg, tp, sl, mode, account_size, topstep_enabled)
            for tp, sl, mode, account_size in itertools.product(
                TP_ATR_GRID, SL_ATR_GRID, MODE_GRID, ACCOUNT_SIZE_GRID
            )
        ]

    df_full = split_by_date(df.copy(), start, end)

    records: List[Dict[str, Any]] = []
    if entry_signals.empty:
        for tp, sl, mode, account_size in itertools.product(
            TP_ATR_GRID, SL_ATR_GRID, MODE_GRID, ACCOUNT_SIZE_GRID
        ):
            records.append(_empty_record(sid, cfg, tp, sl, mode, account_size, topstep_enabled))
        return records

    # Pre-compute signals once per (tp, sl) pair; runner depends on mode.
    signals_by_tp_sl: Dict[Tuple[float, float], pd.DataFrame] = {}
    for tp, sl in itertools.product(TP_ATR_GRID, SL_ATR_GRID):
        signals_by_tp_sl[(tp, sl)] = _attach_tp_sl(
            entry_signals, tp, sl, TICK_SIZES[instrument]
        )

    for mode in MODE_GRID:
        kw = _runner_kwargs(mode, {"mode": mode}, cfg["session_end"])
        for tp, sl in itertools.product(TP_ATR_GRID, SL_ATR_GRID):
            signals_with_sl_tp = signals_by_tp_sl[(tp, sl)]
            runner_signals = apply_runner_to_signals(df_full, signals_with_sl_tp, **kw)

            if runner_signals.empty:
                for account_size in ACCOUNT_SIZE_GRID:
                    records.append(
                        _empty_record(sid, cfg, tp, sl, mode, account_size, topstep_enabled)
                    )
                continue

            for account_size in ACCOUNT_SIZE_GRID:
                stats = _run_rust_backtest(
                    runner_signals,
                    POINT_VALUES[instrument],
                    topstep_enabled,
                    account_size=account_size,
                )
                records.append(
                    {
                        "paper": "asian_paper",
                        "strategy_id": sid,
                        "engine": cfg["engine"],
                        "instrument": instrument,
                        "session": cfg["session"],
                        "target_style": cfg["target_style"],
                        "tp_atr": tp,
                        "sl_atr": sl,
                        "mode": mode,
                        **stats,
                        "topstep_enabled": topstep_enabled,
                        "account_size": account_size,
                    }
                )

    return records


def _build_tasks(args: argparse.Namespace) -> List[Tuple[str, str, str, str, bool]]:
    """Build one task per strategy ID in the requested slice."""
    sids = list_asian_paper_ids()[args.id_start : args.id_end]
    return [
        (sid, args.start_date, args.end_date, args.data_dir, args.topstep)
        for sid in sids
    ]


def _parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Asian-paper parameter grid sweep")
    parser.add_argument("--id-start", type=int, default=0, help="First strategy index (inclusive)")
    parser.add_argument("--id-end", type=int, default=108, help="Last strategy index (exclusive)")
    parser.add_argument("--workers", type=int, default=min(16, mp.cpu_count()), help="Parallel workers")
    parser.add_argument("--output-dir", default="asian_paper_grid_results", help="Directory for CSV output")
    parser.add_argument(
        "--output-file",
        default=None,
        help="Output CSV filename (default: asian_paper_grid_<id_start>_<id_end>.csv)",
    )
    parser.add_argument(
        "--data-dir", default=str(_PROJECT_ROOT / "data"), help="Directory with instrument parquet files"
    )
    parser.add_argument("--start-date", default="2016-06-01", help="Backtest start date")
    parser.add_argument("--end-date", default="2026-05-29", help="Backtest end date")
    parser.add_argument("--topstep", action="store_true", help="Enable Topstep combine rules")
    return parser.parse_args(argv)


def _flush_records(records: List[Dict[str, Any]], out_path: Path, columns: List[str]) -> None:
    df = pd.DataFrame(records, columns=columns)
    df.to_csv(out_path, mode="a", header=False, index=False)


def main(argv=None) -> int:
    args = _parse_args(argv)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_name = args.output_file or f"asian_paper_grid_{args.id_start}_{args.id_end}.csv"
    out_path = out_dir / out_name

    tasks = _build_tasks(args)
    total_ids = len(tasks)
    combos_per_id = len(TP_ATR_GRID) * len(SL_ATR_GRID) * len(MODE_GRID) * len(ACCOUNT_SIZE_GRID)
    total_combos = total_ids * combos_per_id
    print(
        f"[asian_paper_grid_runner] ids {args.id_start}-{args.id_end}, "
        f"{total_ids} ids, {combos_per_id} combos/id, {total_combos} total, "
        f"workers={args.workers}, topstep={args.topstep}, out={out_path}"
    )

    # Extend columns with account_size because the grid varies it.
    columns = list(OUTPUT_COLUMNS_ASIAN_PAPER)
    if "account_size" not in columns:
        columns.append("account_size")

    # Write header once.
    if not out_path.exists():
        pd.DataFrame(columns=columns).to_csv(out_path, index=False)

    completed = 0
    failures = 0
    buffer: List[Dict[str, Any]] = []
    flush_every = 250
    start_t = time.perf_counter()

    workers = min(args.workers, len(tasks)) if tasks else 1
    with ProcessPoolExecutor(max_workers=workers, initializer=_worker_init, initargs=(args.data_dir,)) as pool:
        futures = {pool.submit(_run_asian_paper_id, task): task for task in tasks}
        for future in as_completed(futures):
            try:
                records = future.result()
            except Exception as exc:
                failures += 1
                task = futures[future]
                print(f"  FAILED task {task}: {exc}", file=sys.stderr)
                continue
            buffer.extend(records)
            completed += 1
            if len(buffer) >= flush_every:
                _flush_records(buffer, out_path, columns)
                buffer.clear()
                elapsed = time.perf_counter() - start_t
                print(
                    f"[asian_paper_grid_runner] {completed}/{total_ids} ids "
                    f"({completed * combos_per_id}/{total_combos} combos) "
                    f"done ({elapsed:.1f}s)"
                )

    if buffer:
        _flush_records(buffer, out_path, columns)

    elapsed = time.perf_counter() - start_t
    print(
        f"[asian_paper_grid_runner] done. wrote {out_path} "
        f"completed={completed}/{total_ids} ids failures={failures} elapsed={elapsed:.1f}s"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
