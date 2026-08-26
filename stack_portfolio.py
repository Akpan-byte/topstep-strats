#!/usr/bin/env python3
# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created stack_portfolio.py to simulate stacking Paper-1 and Paper-2
#     strategies under a global Topstep contract cap.
#   - Loads payout_analysis_paper1.csv and payout_analysis_paper2_50k.csv,
#     selects top N strategies per funded account spec (daily_profit_cap_mode=False),
#     regenerates raw trade logs with the same signal logic as payout_analysis.py,
#     and runs combined streams through simulate_topstep_payouts().
#   - Tests greedy-priority and round-robin allocation under 2-contract cap
#     (and 3-contract for 150k specs), plus a lowest-overlap pair.
# WHY: Estimate whether $5k-$10k/week is reachable under realistic Topstep caps.

from __future__ import annotations

import argparse
import io
import multiprocessing as mp
import sys
import time
import traceback
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import Any, Dict, List, Tuple

import numpy as np
import pandas as pd

_PROJECT_ROOT = Path(__file__).resolve().parent
_ENGINE_RUST_ROOT = _PROJECT_ROOT / "engine_rust"

if str(_PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(_PROJECT_ROOT))
if str(_ENGINE_RUST_ROOT / "python") not in sys.path:
    sys.path.insert(0, str(_ENGINE_RUST_ROOT / "python"))

from agnostic_backtest import AgnosticBacktest
from engine_rust.scripts.sweep_engine import (
    PAPER1_INSTRUMENTS,
    PAPER1_RUNNER_MODES,
    PAPER1_SESSIONS,
    PAPER2_INSTRUMENTS,
    PAPER2_SESSIONS,
    TICK_SIZES,
    _attach_tp_sl,
    _build_paper1_entry_cfg,
    _load_instrument_df,
    _runner_kwargs,
    _worker_init,
)
from scripts.runner_exit_numba import apply_runner_to_signals
from topstep_strats.data import get_session_mask, split_by_date
from topstep_strats.strategies.paper1_matrix import generate_signals as generate_paper1_signals
from topstep_strats.strategies.paper2_matrix import (
    generate_signals as generate_paper2_signals,
    get_strategy_config as get_paper2_config,
)
from topstep_payout import simulate_topstep_payouts

_worker_data_dir: str = ""
_worker_entry_cache: Dict[Tuple[str, ...], pd.DataFrame] = {}

PAPER2_RISK_TP_SL: Dict[str, Tuple[float, float]] = {
    "conservative": (1.5, 2.0),
    "aggressive": (3.0, 4.0),
}

ACCOUNT_SPECS = ["50k_standard", "50k_consistency", "150k_standard", "150k_consistency"]

OUTPUT_COLUMNS = [
    "account_spec",
    "daily_profit_cap_mode",
    "stack_size",
    "max_contracts",
    "allocation_method",
    "strategy_list",
    "total_payouts_dollars",
    "avg_payout_per_week",
    "payouts_count",
    "resets",
    "max_drawdown_pct",
    "avg_overlap",
    "trades_executed",
    "win_rate",
    "failed_combines",
    "consistency_violations",
    "final_equity",
    "pair_overlap_raw",
]


def _init_worker(data_dir: str) -> None:
    global _worker_data_dir
    _worker_data_dir = data_dir
    _worker_init(data_dir)
    for instrument in list(PAPER1_INSTRUMENTS) + list(PAPER2_INSTRUMENTS):
        try:
            _load_instrument_df(instrument)
        except Exception as exc:
            print(f"[worker] failed to load {instrument}: {exc}", file=sys.stderr)


def _sid_to_str(sid: Any) -> str:
    try:
        return f"{int(sid):03d}"
    except (ValueError, TypeError):
        return str(sid).zfill(3)


def _paper1_entry_signals(sid: str, instrument: str, session: str, start: str, end: str) -> pd.DataFrame:
    key = ("p1", sid, instrument, session)
    cached = _worker_entry_cache.get(key)
    if cached is not None:
        return cached
    df = _load_instrument_df(instrument)
    cfg = _build_paper1_entry_cfg(sid, instrument, session, start, end)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start, end)
    try:
        entry = generate_paper1_signals(df_s, cfg, simulate_exits=False)
    except Exception as exc:
        print(f"[worker] signal gen failed {key}: {exc}", file=sys.stderr)
        entry = pd.DataFrame(columns=["entry_time", "direction", "entry_price", "atr_value"])
    _worker_entry_cache[key] = entry
    return entry


def _generate_paper1_trades(
    sid: str,
    instrument: str,
    session: str,
    tp: float,
    sl: float,
    mode: str,
    start_date: str,
    end_date: str,
) -> pd.DataFrame:
    entry_signals = _paper1_entry_signals(sid, instrument, session, start_date, end_date)
    if entry_signals.empty:
        return pd.DataFrame()
    signals_with_sl_tp = _attach_tp_sl(entry_signals, tp, sl, TICK_SIZES[instrument])
    base_kwargs = next(kw for name, kw in PAPER1_RUNNER_MODES if name == mode)
    kw = _runner_kwargs(mode, base_kwargs, PAPER1_SESSIONS[session][1])
    df_full = split_by_date(_load_instrument_df(instrument).copy(), start_date, end_date)
    runner_signals = apply_runner_to_signals(df_full, signals_with_sl_tp, **kw)
    if runner_signals.empty:
        return pd.DataFrame()
    engine = AgnosticBacktest(cash=100_000.0, point_value=1.0)
    engine.run_signals(runner_signals, topstep=None)
    rust_trades = engine.trades()
    if rust_trades and len(rust_trades[0]) == 6:
        tdf = pd.DataFrame(
            rust_trades,
            columns=["entry_ts", "exit_ts", "entry_price", "exit_price", "qty", "pnl"],
        )
        tdf["entry_time"] = pd.to_datetime(tdf["entry_ts"], unit="ns", utc=True)
        tdf["exit_time"] = pd.to_datetime(tdf["exit_ts"], unit="ns", utc=True)
    else:
        tdf = runner_signals[["entry_time", "exit_time", "pnl"]].copy()
    tdf["instrument"] = instrument
    return tdf


def _generate_paper2_trades(
    sid: str,
    instrument: str,
    session: str,
    risk_profile: str,
    start_date: str,
    end_date: str,
) -> pd.DataFrame:
    cfg = get_paper2_config(sid)
    cfg.update(
        {
            "instrument": instrument,
            "tick_size": TICK_SIZES[instrument],
            "point_value": {"NQ": 20.0, "ES": 50.0, "YM": 5.0}[instrument],
            "session": session,
            "session_start": PAPER2_SESSIONS[session][0],
            "session_end": PAPER2_SESSIONS[session][1],
            "session_only": True,
            "one_trade_per_day": True,
            "stop_first": True,
            "risk_profile": risk_profile,
            "tp_atr": PAPER2_RISK_TP_SL[risk_profile][0],
            "sl_atr": PAPER2_RISK_TP_SL[risk_profile][1],
        }
    )
    df = _load_instrument_df(instrument)
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), start_date, end_date)
    try:
        signals = generate_paper2_signals(df_s, cfg)
    except Exception as exc:
        print(
            f"[worker] signal gen failed P2 {sid} {instrument} {session} {risk_profile}: {exc}",
            file=sys.stderr,
        )
        return pd.DataFrame()
    if signals.empty:
        return pd.DataFrame()
    engine = AgnosticBacktest(cash=100_000.0, point_value=1.0)
    engine.run_signals(signals, topstep=None)
    rust_trades = engine.trades()
    if rust_trades and len(rust_trades[0]) == 6:
        tdf = pd.DataFrame(
            rust_trades,
            columns=["entry_ts", "exit_ts", "entry_price", "exit_price", "qty", "pnl"],
        )
        tdf["entry_time"] = pd.to_datetime(tdf["entry_ts"], unit="ns", utc=True)
        tdf["exit_time"] = pd.to_datetime(tdf["exit_ts"], unit="ns", utc=True)
    else:
        tdf = signals[["entry_time", "exit_time", "pnl"]].copy()
    tdf["instrument"] = instrument
    return tdf


def generate_trades_for_row(
    row: Dict[str, Any], start_date: str, end_date: str
) -> Tuple[str, pd.DataFrame]:
    source = row.get("source", "paper1")
    sid = _sid_to_str(row["strategy_id"])
    instrument = str(row["instrument"])
    session = str(row["session"])
    if source == "paper1":
        tdf = _generate_paper1_trades(
            sid,
            instrument,
            session,
            float(row["tp"]),
            float(row["sl"]),
            str(row["mode"]),
            start_date,
            end_date,
        )
    else:
        tdf = _generate_paper2_trades(
            sid,
            instrument,
            session,
            str(row["risk_profile"]),
            start_date,
            end_date,
        )
    if tdf.empty:
        return row["strategy_key"], tdf
    tdf = tdf[["entry_time", "exit_time", "pnl", "instrument"]].copy()
    return row["strategy_key"], tdf


def _time_weighted_avg_overlap(taken_df: pd.DataFrame) -> float:
    """Compute average number of simultaneously open trades over time."""
    if taken_df.empty:
        return 0.0
    df = taken_df.copy()
    df["entry_time"] = pd.to_datetime(df["entry_time"], utc=True)
    df["exit_time"] = pd.to_datetime(df["exit_time"], utc=True)
    events = []
    for _, row in df.iterrows():
        events.append((row["entry_time"], 1))
        events.append((row["exit_time"], -1))
    events.sort(key=lambda x: x[0])
    count = 0
    prev_time = None
    weighted = 0.0
    duration = 0.0
    for t, delta in events:
        if prev_time is not None:
            sec = (t - prev_time).total_seconds()
            weighted += count * sec
            duration += sec
        count += delta
        prev_time = t
    return weighted / duration if duration > 0 else 0.0


def simulate_stack(
    trade_logs: List[pd.DataFrame],
    priorities: List[float],
    strategy_keys: List[str],
    max_contracts: int,
    allocation: str = "greedy",
) -> Tuple[pd.DataFrame, float]:
    if not trade_logs:
        return pd.DataFrame(columns=["entry_time", "exit_time", "pnl", "instrument"]), 0.0

    tagged = []
    for tdf, pri, key in zip(trade_logs, priorities, strategy_keys):
        if tdf.empty:
            continue
        t = tdf.copy()
        t["priority"] = pri
        t["strategy_key"] = key
        tagged.append(t)

    df = pd.concat(tagged, ignore_index=True)
    df["entry_time"] = pd.to_datetime(df["entry_time"], utc=True)
    df["exit_time"] = pd.to_datetime(df["exit_time"], utc=True)
    df = df.sort_values(
        ["entry_time", "priority", "strategy_key"], ascending=[True, False, True]
    ).reset_index(drop=True)

    combined: List[Dict[str, Any]] = []
    active: List[Dict[str, Any]] = []
    rr_counter: Dict[str, int] = defaultdict(int)

    i = 0
    n = len(df)
    while i < n:
        row = df.iloc[i]
        active = [a for a in active if a["exit_time"] > row["entry_time"]]
        available = max_contracts - len(active)
        if available <= 0:
            i += 1
            continue

        same_mask = df["entry_time"] == row["entry_time"]
        same_idx = df.index[same_mask]
        same = df.loc[same_idx].copy()
        j = i + len(same_idx)

        if allocation == "greedy":
            same = same.sort_values(["priority", "strategy_key"], ascending=[False, True])
            taken = same.head(available)
        elif allocation == "round_robin":
            same["rr_count"] = same["strategy_key"].map(rr_counter)
            same = same.sort_values(
                ["rr_count", "priority", "strategy_key"], ascending=[True, False, True]
            )
            taken = same.head(available)
            for _, trow in taken.iterrows():
                rr_counter[trow["strategy_key"]] += 1
        else:
            raise ValueError(f"Unknown allocation: {allocation}")

        for _, trow in taken.iterrows():
            combined.append(
                {
                    "entry_time": trow["entry_time"],
                    "exit_time": trow["exit_time"],
                    "pnl": trow["pnl"],
                    "instrument": trow["instrument"],
                }
            )
            active.append({"exit_time": trow["exit_time"], "strategy_key": trow["strategy_key"]})

        i = j

    result_df = pd.DataFrame(combined)
    avg_overlap = _time_weighted_avg_overlap(result_df)
    return result_df, avg_overlap


def pair_overlap(tdf1: pd.DataFrame, tdf2: pd.DataFrame) -> float:
    """Return time-both-open / time-either-open overlap between two trade logs."""
    if tdf1.empty or tdf2.empty:
        return 0.0
    t1 = tdf1[["entry_time", "exit_time"]].copy()
    t2 = tdf2[["entry_time", "exit_time"]].copy()
    for col in ("entry_time", "exit_time"):
        t1[col] = pd.to_datetime(t1[col], utc=True)
        t2[col] = pd.to_datetime(t2[col], utc=True)
    t1["day"] = t1["entry_time"].dt.date
    t2["day"] = t2["entry_time"].dt.date

    overlap_sec = 0.0
    common_days = set(t1["day"]).intersection(t2["day"])
    for day in common_days:
        d1 = t1[t1["day"] == day]
        d2 = t2[t2["day"] == day]
        for _, r1 in d1.iterrows():
            mask = (d2["entry_time"] < r1["exit_time"]) & (d2["exit_time"] > r1["entry_time"])
            for _, r2 in d2[mask].iterrows():
                ostart = max(r1["entry_time"], r2["entry_time"])
                oend = min(r1["exit_time"], r2["exit_time"])
                if ostart < oend:
                    overlap_sec += (oend - ostart).total_seconds()

    total_sec = (
        (t1["exit_time"] - t1["entry_time"]).dt.total_seconds().sum()
        + (t2["exit_time"] - t2["entry_time"]).dt.total_seconds().sum()
    )
    return overlap_sec / total_sec if total_sec > 0 else 0.0


def build_strategy_key(row: Dict[str, Any]) -> str:
    source = row.get("source", "paper1")
    if source == "paper1":
        return (
            f"{source}|{_sid_to_str(row['strategy_id'])}|{row['instrument']}|"
            f"{row['session']}|{row['tp']}|{row['sl']}|{row['mode']}"
        )
    return (
        f"{source}|{_sid_to_str(row['strategy_id'])}|{row['instrument']}|"
        f"{row['session']}|{row['risk_profile']}"
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Portfolio/stacking simulator under Topstep contract caps.")
    parser.add_argument("--results-dir", default=str(_PROJECT_ROOT / "gh_results" / "rust_sweep_v4"))
    parser.add_argument("--data-dir", default=str(_PROJECT_ROOT / "data"))
    parser.add_argument(
        "--output",
        default=str(_PROJECT_ROOT / "gh_results" / "rust_sweep_v4" / "stack_portfolio_results.csv"),
    )
    parser.add_argument(
        "--log",
        default=str(_PROJECT_ROOT / "gh_results" / "rust_sweep_v4" / "stack_portfolio.log"),
    )
    parser.add_argument("--workers", type=int, default=min(16, mp.cpu_count()))
    parser.add_argument("--top-n-max", type=int, default=10)
    parser.add_argument("--ns", type=int, nargs="+", default=[5, 10])
    parser.add_argument("--start-date", default="2016-06-01")
    parser.add_argument("--end-date", default="2026-05-29")
    return parser


def main(argv=None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    results_dir = Path(args.results_dir)
    df1 = pd.read_csv(results_dir / "payout_analysis_paper1.csv")
    df1["source"] = "paper1"
    df2 = pd.read_csv(results_dir / "payout_analysis_paper2_50k.csv")
    df2["source"] = "paper2"
    combined = pd.concat([df1, df2], ignore_index=True)
    combined["daily_profit_cap_mode"] = combined["daily_profit_cap_mode"].astype(bool)
    combined["avg_payout_per_week"] = pd.to_numeric(
        combined["avg_payout_per_week"], errors="coerce"
    )

    # Drop duplicates that arise when the same signal config appears in both the
    # 50k and 150k source sweeps (identical trades, identical payout for a given
    # account_spec). Keep the row with the highest avg_payout_per_week.
    combined["_strategy_key"] = combined.apply(build_strategy_key, axis=1)
    combined = combined.sort_values("avg_payout_per_week", ascending=False)
    combined = combined.drop_duplicates(
        ["_strategy_key", "account_spec", "daily_profit_cap_mode"], keep="first"
    )

    needed: Dict[str, Dict[str, Any]] = {}
    top_by_spec: Dict[str, pd.DataFrame] = {}
    for spec in ACCOUNT_SPECS:
        df_spec = combined[
            (combined["account_spec"] == spec) & (combined["daily_profit_cap_mode"] == False)
        ]
        top = df_spec.sort_values("avg_payout_per_week", ascending=False).head(args.top_n_max)
        top_by_spec[spec] = top
        for _, row in top.iterrows():
            rec = row.to_dict()
            key = build_strategy_key(rec)
            rec["strategy_key"] = key
            needed[key] = rec
        print(
            f"[stack_portfolio] {spec}: top {len(top)} strategies "
            f"({len(needed)} unique across specs so far)"
        )

    if not needed:
        print("[stack_portfolio] no strategies selected; exiting", file=sys.stderr)
        return 1

    tasks = list(needed.values())
    print(f"[stack_portfolio] regenerating trade logs for {len(tasks)} unique strategies...")

    trade_log_cache: Dict[str, pd.DataFrame] = {}
    failures = 0
    completed = 0
    workers = min(args.workers, len(tasks)) if tasks else 1
    gen_start = time.perf_counter()

    with ProcessPoolExecutor(
        max_workers=workers, initializer=_init_worker, initargs=(args.data_dir,)
    ) as pool:
        futures = {
            pool.submit(generate_trades_for_row, task, args.start_date, args.end_date): task
            for task in tasks
        }
        for future in as_completed(futures):
            try:
                key, tdf = future.result()
                trade_log_cache[key] = tdf
                completed += 1
            except Exception as exc:
                failures += 1
                task = futures[future]
                print(
                    f"[stack_portfolio] FAILED trade generation for {task['strategy_key']}: {exc}",
                    file=sys.stderr,
                )
                traceback.print_exc(file=sys.stderr)
                trade_log_cache[task["strategy_key"]] = pd.DataFrame()

    print(
        f"[stack_portfolio] trade logs generated in {time.perf_counter() - gen_start:.1f}s "
        f"(completed={completed}/{len(tasks)} failures={failures})"
    )

    result_rows: List[Dict[str, Any]] = []
    for spec in ACCOUNT_SPECS:
        top = top_by_spec[spec]
        strategies = top.to_dict("records")
        for rec in strategies:
            rec["strategy_key"] = build_strategy_key(rec)
        priorities = [float(r["avg_payout_per_week"]) for r in strategies]
        logs = [trade_log_cache.get(r["strategy_key"], pd.DataFrame()) for r in strategies]
        keys = [r["strategy_key"] for r in strategies]

        contract_caps = [2]
        if "150k" in spec:
            contract_caps.append(3)

        for n in args.ns:
            if n > len(strategies):
                continue
            sel_logs = logs[:n]
            sel_priors = priorities[:n]
            sel_keys = keys[:n]
            for max_contracts in contract_caps:
                for allocation in ("greedy", "round_robin"):
                    stack_df, avg_overlap = simulate_stack(
                        sel_logs, sel_priors, sel_keys, max_contracts, allocation
                    )
                    if stack_df.empty:
                        continue
                    payout = simulate_topstep_payouts(
                        stack_df, spec, contracts=1, daily_profit_cap_mode=False
                    )
                    result_rows.append(
                        {
                            "account_spec": spec,
                            "daily_profit_cap_mode": False,
                            "stack_size": n,
                            "max_contracts": max_contracts,
                            "allocation_method": allocation,
                            "strategy_list": ";".join(sel_keys),
                            "total_payouts_dollars": payout["total_payouts_dollars"],
                            "avg_payout_per_week": payout["avg_payout_per_week"],
                            "payouts_count": payout["payouts_count"],
                            "resets": payout["resets"],
                            "max_drawdown_pct": payout["max_drawdown_pct"],
                            "avg_overlap": avg_overlap,
                            "trades_executed": payout["trades_executed"],
                            "win_rate": payout["win_rate"],
                            "failed_combines": payout["failed_combines"],
                            "consistency_violations": payout["consistency_violations"],
                            "final_equity": payout["final_equity"],
                            "pair_overlap_raw": np.nan,
                        }
                    )

        # Best pair: lowest raw overlap among top 10 strategies.
        if len(strategies) >= 2:
            top_n_for_pair = min(10, len(strategies))
            pair_keys = keys[:top_n_for_pair]
            pair_logs = logs[:top_n_for_pair]
            best_pair = None
            best_overlap = float("inf")
            for i in range(top_n_for_pair):
                for j in range(i + 1, top_n_for_pair):
                    ov = pair_overlap(pair_logs[i], pair_logs[j])
                    if ov < best_overlap:
                        best_overlap = ov
                        best_pair = (i, j)
            if best_pair is not None:
                i, j = best_pair
                sel_logs = [pair_logs[i], pair_logs[j]]
                sel_priors = [priorities[i], priorities[j]]
                sel_keys = [pair_keys[i], pair_keys[j]]
                stack_df, avg_overlap = simulate_stack(sel_logs, sel_priors, sel_keys, 2, "greedy")
                if not stack_df.empty:
                    payout = simulate_topstep_payouts(
                        stack_df, spec, contracts=1, daily_profit_cap_mode=False
                    )
                    result_rows.append(
                        {
                            "account_spec": spec,
                            "daily_profit_cap_mode": False,
                            "stack_size": 2,
                            "max_contracts": 2,
                            "allocation_method": "best_pair_lowest_overlap",
                            "strategy_list": ";".join(sel_keys),
                            "total_payouts_dollars": payout["total_payouts_dollars"],
                            "avg_payout_per_week": payout["avg_payout_per_week"],
                            "payouts_count": payout["payouts_count"],
                            "resets": payout["resets"],
                            "max_drawdown_pct": payout["max_drawdown_pct"],
                            "avg_overlap": avg_overlap,
                            "trades_executed": payout["trades_executed"],
                            "win_rate": payout["win_rate"],
                            "failed_combines": payout["failed_combines"],
                            "consistency_violations": payout["consistency_violations"],
                            "final_equity": payout["final_equity"],
                            "pair_overlap_raw": best_overlap,
                        }
                    )

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    out_df = pd.DataFrame(result_rows)
    for col in OUTPUT_COLUMNS:
        if col not in out_df.columns:
            out_df[col] = np.nan
    out_df[OUTPUT_COLUMNS].to_csv(output_path, index=False)
    print(f"[stack_portfolio] wrote {output_path} rows={len(out_df)}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    parser = _build_parser()
    args = parser.parse_args()
    log_path = Path(args.log)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_buffer = io.StringIO()
    original_stdout = sys.stdout
    original_stderr = sys.stderr
    sys.stdout = log_buffer
    sys.stderr = log_buffer
    try:
        rc = main()
    finally:
        sys.stdout = original_stdout
        sys.stderr = original_stderr
        log_path.write_text(log_buffer.getvalue())
        print(f"[stack_portfolio] log saved to {log_path}")
    sys.exit(rc)
