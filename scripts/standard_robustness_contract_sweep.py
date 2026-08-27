#!/usr/bin/env python3
"""
Standard-account robustness optimization + eval contract-size sweep.

For standard accounts (consistency_rule=False):
  1. Robustness objective: select strategies that maximize bootstrap weekly payout.
     - First filter top 20% by raw weekly payout.
     - Run 2,000 bootstrap draws on survivors to rank by bootstrap mean.
     - Run full 20,000 MC + 20,000 bootstrap on the final selected stack.
  2. Eval contract sweep: for contract sizes 1-5, find the fastest eval-pass config.
     - For each contract size, re-score all strategies raw.
     - Pick best London + best NY by median eval pass days (pass rate >= 50%).
     - Run full 20,000 MC + 20,000 bootstrap on each selected stack.
"""

# CHANGE_SUMMARY
# 2026-08-26  kilo
#   - Added robustness objective: maximize bootstrap weekly payout on standard accounts.
#   - Added eval contract-size sweep (1-5 contracts) for fastest eval pass days.
#   - Uses reduced bootstrap draws (2,000) for candidate ranking, then full 20k/20k on finals.
# WHY: High raw-payout strategies can be fragile under resampling; bootstrap-aware
#      selection produces more reliable configs. Eval-pass speed varies with sizing.

import os
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Any, Dict, List, Tuple

import numpy as np
import numba as nb
import pandas as pd

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_ENGINE_RUST_ROOT = _PROJECT_ROOT / "engine_rust"

sys.path.insert(0, str(_PROJECT_ROOT))
sys.path.insert(0, str(_ENGINE_RUST_ROOT / "python"))

from stack_portfolio import (
    _init_worker,
    build_strategy_key,
    generate_trades_for_row,
)
from topstep_payout import (
    TOPSTEP_SPECS,
    _net_pnl,
    _resolve_account_spec,
    _to_est_day,
)

DATA_DIR = os.environ.get("TOPSTEP_DATA_DIR", str(_PROJECT_ROOT / "data"))
RESULTS_CSV = (
    _PROJECT_ROOT / "gh_results" / "rust_sweep_v4" / "payout_analysis_comprehensive.csv"
)
OUT_CSV = _PROJECT_ROOT / "standard_robustness_contract_sweep_report.csv"
START_DATE = "2016-06-01"
END_DATE = "2026-05-29"
N_MC_FULL = int(os.environ.get("STD_ROBUST_N_MC", "2000"))
N_BOOT_FULL = int(os.environ.get("STD_ROBUST_N_BOOT", "2000"))
N_BOOT_RANK = int(os.environ.get("STD_ROBUST_N_BOOT_RANK", "200"))
RANDOM_SEED = 42
ROBUST_TOP_FRAC = 0.20

ACCOUNT_SPECS = ["50k_standard", "150k_standard"]
EVAL_CONTRACT_SIZES = [1, 2, 3, 4, 5]
OBJECTIVES = ["robustness"] + [f"eval_{c}ctr" for c in EVAL_CONTRACT_SIZES]

# Pre-filter candidates using the CSV's pre-computed avg_payout_per_week before
# regenerating trade logs. This avoids regenerating trades for the full candidate
# set on every matrix cell, which is prohibitively slow on GitHub runners.
PRE_FILTER_TOP_N = int(os.environ.get("STD_ROBUST_PRE_FILTER", "30"))


def get_spec_dict(spec_name: str) -> dict:
    spec = TOPSTEP_SPECS[spec_name].copy()
    spec["consistency_rule"] = False
    return spec


def _score_row(args: Tuple[dict, dict, int]) -> Dict[str, Any]:
    """Score a single strategy row; intended for ProcessPoolExecutor."""
    row_dict, spec, max_contracts = args
    key = row_dict["strategy_key"]
    try:
        _, trades = generate_trades_for_row(row_dict, START_DATE, END_DATE)
        if trades.empty:
            return None
        trades_scaled = trades.copy()
        trades_scaled["pnl"] = trades_scaled["pnl"] * max_contracts
        sim = simulate_topstep_payouts_detailed(trades_scaled, spec, contracts=max_contracts)
        return {
            **row_dict,
            "trades": trades,
            "raw_weekly": sim["avg_payout_per_week"],
            "raw_eval_days": sim["eval_pass_days"],
            "raw_pass_rate": sim["pass_rate"],
        }
    except Exception as e:
        print(f"[warn] failed {key}: {e}")
        return None


# ---------------------------------------------------------------------------
# Detailed simulator
# ---------------------------------------------------------------------------
def simulate_topstep_payouts_detailed(
    trades_df: pd.DataFrame,
    account_spec: Any,
    contracts: int = 1,
) -> Dict[str, Any]:
    if trades_df is None or trades_df.empty:
        raise ValueError("trades_df must not be empty")

    spec = _resolve_account_spec(account_spec)
    profit_target = float(spec["profit_target"])
    daily_dd = float(spec["daily_dd"])
    trailing_dd = float(spec["trailing_dd"])
    payout_cap = float(spec["payout_cap"])
    min_winning_days = int(spec["min_winning_days"])
    consistency_rule = bool(spec["consistency_rule"])
    payout_pct = float(spec["payout_pct_of_balance"])
    initial_capital = float(spec["initial_capital"])

    df = trades_df.copy()
    for col in ("entry_time", "exit_time"):
        df[col] = pd.to_datetime(df[col])
    df = df.sort_values("exit_time").reset_index(drop=True)
    df["net_pnl"] = _net_pnl(df, contracts)
    df["day"] = _to_est_day(df["exit_time"])

    cash = initial_capital
    high_water = initial_capital
    daily_start = initial_capital
    daily_pnl = 0.0
    total_pnl_combine = 0.0
    winning_days_current = 0
    day_pnl_current: Dict[Any, float] = {}

    resets = 0
    failed_combines = 0
    payouts_count = 0
    daily_limit_hits = 0
    consistency_violations_total = 0
    winning_days_total = 0
    total_payouts_dollars = 0.0

    trades_executed = 0
    winning_trades = 0
    total_trade_pnl = 0.0
    max_drawdown_pct = 0.0

    trading_days: set = set()
    daily_equity: Dict[Any, float] = {}
    combines: List[Dict[str, Any]] = []
    current_combine = {
        "start_day": None,
        "trading_days": set(),
        "daily_limit_hits": 0,
        "payout": False,
        "days_to_payout": None,
    }

    prev_day = None

    def _reset_account():
        nonlocal cash, high_water, daily_start, daily_pnl, total_pnl_combine
        nonlocal winning_days_current, day_pnl_current
        cash = initial_capital
        high_water = initial_capital
        daily_start = initial_capital
        daily_pnl = 0.0
        total_pnl_combine = 0.0
        winning_days_current = 0
        day_pnl_current = {}

    def _consistency_violated():
        if not consistency_rule or total_pnl_combine <= 0:
            return False
        threshold = 0.4 * total_pnl_combine
        return any(dpnl > threshold for dpnl in day_pnl_current.values())

    def _finalize_day(day):
        nonlocal cash, high_water, daily_start, daily_pnl, total_pnl_combine
        nonlocal winning_days_current, winning_days_total
        nonlocal consistency_violations_total
        nonlocal total_payouts_dollars, payouts_count, resets
        nonlocal current_combine

        if daily_pnl > 0:
            winning_days_current += 1
            winning_days_total += 1

        day_pnl_current[day] = daily_pnl

        if (
            cash >= initial_capital + profit_target
            and winning_days_current >= min_winning_days
            and not _consistency_violated()
        ):
            withdrawal = min(cash * payout_pct, payout_cap)
            total_payouts_dollars += withdrawal
            payouts_count += 1
            resets += 1
            current_combine["payout"] = True
            current_combine["days_to_payout"] = len(current_combine["trading_days"])
            combines.append(current_combine)
            _reset_account()
            current_combine = {
                "start_day": None,
                "trading_days": set(),
                "daily_limit_hits": 0,
                "payout": False,
                "days_to_payout": None,
            }
        elif (
            consistency_rule
            and cash >= initial_capital + profit_target
            and winning_days_current >= min_winning_days
            and _consistency_violated()
        ):
            consistency_violations_total += 1

        daily_equity[day] = cash

    for row in df.itertuples(index=False):
        day = row.day
        if day != prev_day:
            if prev_day is not None:
                _finalize_day(prev_day)
            daily_start = cash
            daily_pnl = 0.0
            prev_day = day
            if current_combine["start_day"] is None:
                current_combine["start_day"] = day

        current_combine["trading_days"].add(day)
        trading_days.add(day)
        trade_net = float(row.net_pnl)

        if cash + trade_net < daily_start - daily_dd:
            daily_limit_hits += 1
            current_combine["daily_limit_hits"] += 1
            continue

        cash += trade_net
        daily_pnl += trade_net
        total_pnl_combine += trade_net
        total_trade_pnl += trade_net
        trades_executed += 1

        if trade_net > 0:
            winning_trades += 1

        if cash > high_water:
            high_water = cash

        dd_pct = (high_water - cash) / high_water if high_water > 0 else 0.0
        if dd_pct > max_drawdown_pct:
            max_drawdown_pct = dd_pct

        if cash < high_water - trailing_dd:
            resets += 1
            failed_combines += 1
            current_combine["payout"] = False
            combines.append(current_combine)
            _reset_account()
            current_combine = {
                "start_day": None,
                "trading_days": set(),
                "daily_limit_hits": 0,
                "payout": False,
                "days_to_payout": None,
            }

    if prev_day is not None:
        _finalize_day(prev_day)

    if current_combine["start_day"] is not None and not current_combine["payout"]:
        combines.append(current_combine)

    trading_days_count = len(trading_days)

    paid_combines = [c for c in combines if c["payout"]]
    eval_pass_days = float(np.median([c["days_to_payout"] for c in paid_combines])) if paid_combines else np.nan
    pass_rate = len(paid_combines) / len(combines) if combines else 0.0

    return {
        "avg_payout_per_week": total_payouts_dollars / (trading_days_count / 5.0) if trading_days_count else 0.0,
        "total_payouts_dollars": total_payouts_dollars,
        "payouts_count": payouts_count,
        "resets": resets,
        "failed_combines": failed_combines,
        "daily_limit_hits": daily_limit_hits,
        "consistency_violations": consistency_violations_total,
        "win_rate": (winning_trades / trades_executed * 100.0) if trades_executed else 0.0,
        "max_drawdown_pct": max_drawdown_pct * 100.0,
        "trades_executed": trades_executed,
        "eval_pass_days": eval_pass_days,
        "pass_rate": pass_rate * 100.0,
        "daily_pnl": df.groupby("day")["net_pnl"].sum().sort_index(),
    }


# ---------------------------------------------------------------------------
# Fast daily-level Numba simulator
# ---------------------------------------------------------------------------
@nb.njit
def _simulate_daily_path(
    daily_pnl: np.ndarray,
    profit_target: float,
    daily_dd: float,
    trailing_dd: float,
    payout_cap: float,
    min_winning_days: int,
    payout_pct: float,
    initial_capital: float,
) -> np.ndarray:
    n_days = len(daily_pnl)
    cash = initial_capital
    high_water = initial_capital
    total_pnl_combine = 0.0
    winning_days_current = 0

    resets = 0
    failed_combines = 0
    payouts_count = 0
    total_payouts = 0.0

    paid_days = np.empty(n_days, dtype=np.float64)
    paid_count = 0

    for i in range(n_days):
        dpnl = daily_pnl[i]
        if dpnl > 0.0:
            winning_days_current += 1

        cash += dpnl
        total_pnl_combine += dpnl

        if cash > high_water:
            high_water = cash

        if (
            cash >= initial_capital + profit_target
            and winning_days_current >= min_winning_days
        ):
            withdrawal = min(cash * payout_pct, payout_cap)
            total_payouts += withdrawal
            payouts_count += 1
            resets += 1
            paid_days[paid_count] = float(i + 1)
            paid_count += 1
            cash = initial_capital
            high_water = initial_capital
            total_pnl_combine = 0.0
            winning_days_current = 0

        if cash < high_water - trailing_dd:
            resets += 1
            failed_combines += 1
            cash = initial_capital
            high_water = initial_capital
            total_pnl_combine = 0.0
            winning_days_current = 0

    if paid_count > 0:
        eval_pass_days = np.median(paid_days[:paid_count])
    else:
        eval_pass_days = np.nan

    weekly_payout = (total_payouts / (n_days / 5.0)) if n_days > 0 else 0.0

    out = np.empty(5, dtype=np.float64)
    out[0] = weekly_payout
    out[1] = eval_pass_days
    out[2] = total_payouts
    out[3] = float(payouts_count)
    out[4] = float(failed_combines)
    return out


METRIC_KEYS = ["weekly_payout", "eval_pass_days", "total_payouts", "payouts_count", "failed_combines"]


def simulate_daily_path(daily_pnl: np.ndarray, spec: dict) -> Dict[str, float]:
    arr = _simulate_daily_path(
        np.asarray(daily_pnl, dtype=np.float64),
        float(spec["profit_target"]),
        float(spec["daily_dd"]),
        float(spec["trailing_dd"]),
        float(spec["payout_cap"]),
        int(spec["min_winning_days"]),
        float(spec["payout_pct_of_balance"]),
        float(spec["initial_capital"]),
    )
    return {k: float(v) for k, v in zip(METRIC_KEYS, arr)}


def mc_draw(daily_pnl: np.ndarray, spec: dict, seed: int) -> Dict[str, float]:
    rng = np.random.default_rng(seed)
    n_days = len(daily_pnl)
    idx = rng.integers(0, n_days, size=n_days)
    return simulate_daily_path(daily_pnl[idx], spec)


def bootstrap_draw(daily_pnl: np.ndarray, spec: dict, seed: int, block_size: int = 5) -> Dict[str, float]:
    rng = np.random.default_rng(seed)
    n_days = len(daily_pnl)
    n_blocks = int(np.ceil(n_days / block_size))
    sampled_starts = rng.integers(0, n_days - block_size + 1, size=n_blocks)
    idx = np.empty(n_blocks * block_size, dtype=np.int64)
    for i, start in enumerate(sampled_starts):
        idx[i * block_size : (i + 1) * block_size] = np.arange(start, start + block_size)
    idx = idx[:n_days]
    return simulate_daily_path(daily_pnl[idx], spec)


def run_mc_boot(daily_pnl: np.ndarray, spec: dict, n_mc: int, n_boot: int, seed_offset: int = 0) -> Tuple[List[Dict], List[Dict]]:
    mc = [mc_draw(daily_pnl, spec, RANDOM_SEED + seed_offset + i) for i in range(n_mc)]
    boot = [bootstrap_draw(daily_pnl, spec, RANDOM_SEED + seed_offset + n_mc + i) for i in range(n_boot)]
    return mc, boot


def summarize_series(values: np.ndarray) -> Dict[str, float]:
    clean = np.array(values, dtype=float)
    clean = clean[np.isfinite(clean)]
    if len(clean) == 0:
        return {k: np.nan for k in ["mean", "median", "min", "max", "p5", "p25", "p75", "p95", "std"]}
    return {
        "mean": float(np.mean(clean)),
        "median": float(np.median(clean)),
        "min": float(np.min(clean)),
        "max": float(np.max(clean)),
        "p5": float(np.percentile(clean, 5)),
        "p25": float(np.percentile(clean, 25)),
        "p75": float(np.percentile(clean, 75)),
        "p95": float(np.percentile(clean, 95)),
        "std": float(np.std(clean)),
    }


def cluster_label(values: np.ndarray, n_clusters: int = 3) -> str:
    clean = np.array(values, dtype=float)
    clean = clean[np.isfinite(clean)]
    if len(clean) < n_clusters:
        return "n/a"
    try:
        from sklearn.cluster import KMeans
        km = KMeans(n_clusters=n_clusters, random_state=RANDOM_SEED, n_init="auto")
        labels = km.fit_predict(clean.reshape(-1, 1))
        centers = sorted(km.cluster_centers_.flatten())
        return "; ".join(f"C{i+1}~{c:.1f}" for i, c in enumerate(centers))
    except Exception:
        counts, edges = np.histogram(clean, bins=5)
        return "; ".join(f"[{edges[i]:.1f},{edges[i+1]:.1f}):{counts[i]}" for i in range(len(counts)))


def add_distribution_stats(row: Dict[str, Any], draws: List[Dict[str, float]], prefix: str) -> Dict[str, Any]:
    for metric in ["weekly_payout", "eval_pass_days", "total_payouts", "failed_combines"]:
        vals = np.array([d[metric] for d in draws])
        stats = summarize_series(vals)
        for k, v in stats.items():
            row[f"{prefix}_{metric}_{k}"] = v
        row[f"{prefix}_{metric}_clusters"] = cluster_label(vals)
    return row


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main(objective: str = "all"):
    print("[main] initializing worker...")
    _init_worker(DATA_DIR)

    print(f"[main] loading {RESULTS_CSV}...")
    df = pd.read_csv(RESULTS_CSV)
    df["daily_profit_cap_mode"] = df["daily_profit_cap_mode"].astype(bool)
    df["strategy_key"] = df.apply(build_strategy_key, axis=1)
    df = df[df["daily_profit_cap_mode"] == False].copy()

    print(f"[main] total candidate rows: {len(df)}")
    records = []

    for spec_name in ACCOUNT_SPECS:
        print(f"\n[main] ===== {spec_name} =====")
        spec = get_spec_dict(spec_name)
        max_contracts = spec["max_contracts"]
        base = df[df["account_spec"] == spec_name].copy()

        # Pre-generate trades and raw-score every strategy with max_contracts.
        # Use the CSV's pre-computed avg_payout_per_week to narrow the field
        # before regenerating trade logs; this keeps the matrix cells fast.
        # Pre-filter per session so London and NY are both represented.
        print("[main] pre-filtering candidates...")
        per_session = PRE_FILTER_TOP_N // 2
        base = (
            base.sort_values("avg_payout_per_week", ascending=False)
            .groupby("session", group_keys=False)
            .head(per_session)
            .copy()
        )
        print(f"[main] raw-scoring top {len(base)} strategies (per-session pre-filter)...")
        row_dicts = base.to_dict("records")
        workers = min(2, os.cpu_count() or 1)
        tasks = [(row, spec, max_contracts) for row in row_dicts]
        scored: List[Dict[str, Any]] = []
        with ProcessPoolExecutor(max_workers=workers, initializer=_init_worker, initargs=(DATA_DIR,)) as pool:
            for result in pool.map(_score_row, tasks):
                if result is not None:
                    scored.append(result)

        scored_df = pd.DataFrame(scored)
        print(f"[main] successfully scored {len(scored_df)} strategies")
        if scored_df.empty:
            raise RuntimeError(
                f"No strategies were successfully scored for {spec_name}. "
                "This usually means the Rust extension is not built or the trade-log generation failed."
            )

        # ------------------------------------------------------------------
        # Robustness objective: top 20% by raw, then rank by bootstrap payout
        # ------------------------------------------------------------------
        if objective in ("all", "robustness"):
            print("[main] running robustness selection...")
            top_n = max(1, int(len(scored_df) * ROBUST_TOP_FRAC))
            # Pick the top-N candidates within each session so that London and NY
            # are both represented regardless of which session dominates globally.
            robust_candidates = (
                scored_df.sort_values("raw_weekly", ascending=False)
                .groupby("session", group_keys=False)
                .head(top_n)
                .copy()
            )
    
            # Bootstrap ranking on survivors (reduced draws)
            boot_scores = []
            for _, row in robust_candidates.iterrows():
                trades_scaled = row["trades"].copy()
                trades_scaled["pnl"] = trades_scaled["pnl"] * max_contracts
                sim = simulate_topstep_payouts_detailed(trades_scaled, spec, contracts=max_contracts)
                daily_pnl = sim["daily_pnl"].values.astype(np.float64)
                _, boot = run_mc_boot(daily_pnl, spec, n_mc=0, n_boot=N_BOOT_RANK, seed_offset=0)
                boot_weekly = np.mean([d["weekly_payout"] for d in boot])
                boot_scores.append({
                    **row.to_dict(),
                    "boot_weekly_rank": boot_weekly,
                    "daily_pnl": sim["daily_pnl"],
                })
            boot_df = pd.DataFrame(boot_scores)

            london_df = boot_df[boot_df["session"] == "London"]
            ny_df = boot_df[boot_df["session"] == "NY"]
            if london_df.empty or ny_df.empty:
                raise RuntimeError(
                    f"Robustness selection needs both London and NY candidates, got "
                    f"London={len(london_df)} NY={len(ny_df)} (scored_df sessions: {scored_df['session'].unique().tolist()})"
                )
            robust_london = london_df.sort_values("boot_weekly_rank", ascending=False).iloc[0]
            robust_ny = ny_df.sort_values("boot_weekly_rank", ascending=False).iloc[0]
    
            combined = pd.concat([robust_london["trades"], robust_ny["trades"]], ignore_index=True)
            combined["pnl"] = combined["pnl"] * max_contracts
            combined = combined.sort_values("exit_time").reset_index(drop=True)
            sim = simulate_topstep_payouts_detailed(combined, spec, contracts=max_contracts)
            daily_pnl = sim["daily_pnl"].values.astype(np.float64)
            mc, boot = run_mc_boot(daily_pnl, spec, N_MC_FULL, N_BOOT_FULL, seed_offset=100000)
    
            row = {
                "account_spec": spec_name,
                "objective": "robustness",
                "london_strategy": robust_london["strategy_key"],
                "ny_strategy": robust_ny["strategy_key"],
                "contracts": max_contracts,
                "raw_weekly_payout": sim["avg_payout_per_week"],
                "raw_eval_pass_days": sim["eval_pass_days"],
                "raw_pass_rate": sim["pass_rate"],
                "raw_win_rate": sim["win_rate"],
                "raw_max_drawdown_pct": sim["max_drawdown_pct"],
            }
            row = add_distribution_stats(row, mc, "mc")
            row = add_distribution_stats(row, boot, "boot")
            records.append(row)
            print(f"[main] robustness: London={robust_london['strategy_key']} | NY={robust_ny['strategy_key']} | raw=${sim['avg_payout_per_week']:.0f}")

        # ------------------------------------------------------------------
        # Eval contract sweep: 1-5 contracts
        # ------------------------------------------------------------------
        for contracts in EVAL_CONTRACT_SIZES:
            if objective not in ("all", f"eval_{contracts}ctr"):
                continue
            print(f"[main] eval contract sweep: {contracts} contracts...")
            eval_scores = []
            for _, row in scored_df.iterrows():
                trades_scaled = row["trades"].copy()
                trades_scaled["pnl"] = trades_scaled["pnl"] * contracts
                sim = simulate_topstep_payouts_detailed(trades_scaled, spec, contracts=contracts)
                eval_scores.append({
                    **row.to_dict(),
                    "eval_weekly": sim["avg_payout_per_week"],
                    "eval_days": sim["eval_pass_days"],
                    "eval_pass_rate": sim["pass_rate"],
                    "daily_pnl": sim["daily_pnl"],
                })
            eval_df = pd.DataFrame(eval_scores)
            eval_cands = eval_df[eval_df["eval_pass_rate"] >= 50.0].copy()
            if eval_cands.empty:
                eval_cands = eval_df.copy()

            london_df = eval_cands[eval_cands["session"] == "London"]
            ny_df = eval_cands[eval_cands["session"] == "NY"]
            if london_df.empty or ny_df.empty:
                raise RuntimeError(
                    f"Eval contract sweep needs both London and NY candidates, got "
                    f"London={len(london_df)} NY={len(ny_df)} (scored_df sessions: {scored_df['session'].unique().tolist()})"
                )
            eval_london = london_df.sort_values(["eval_days", "eval_pass_rate"], ascending=[True, False]).iloc[0]
            eval_ny = ny_df.sort_values(["eval_days", "eval_pass_rate"], ascending=[True, False]).iloc[0]

            combined = pd.concat([eval_london["trades"], eval_ny["trades"]], ignore_index=True)
            combined["pnl"] = combined["pnl"] * contracts
            combined = combined.sort_values("exit_time").reset_index(drop=True)
            sim = simulate_topstep_payouts_detailed(combined, spec, contracts=contracts)
            daily_pnl = sim["daily_pnl"].values.astype(np.float64)
            mc, boot = run_mc_boot(daily_pnl, spec, N_MC_FULL, N_BOOT_FULL, seed_offset=200000 + contracts * 10000)

            row = {
                "account_spec": spec_name,
                "objective": f"eval_{contracts}ctr",
                "london_strategy": eval_london["strategy_key"],
                "ny_strategy": eval_ny["strategy_key"],
                "contracts": contracts,
                "raw_weekly_payout": sim["avg_payout_per_week"],
                "raw_eval_pass_days": sim["eval_pass_days"],
                "raw_pass_rate": sim["pass_rate"],
                "raw_win_rate": sim["win_rate"],
                "raw_max_drawdown_pct": sim["max_drawdown_pct"],
            }
            row = add_distribution_stats(row, mc, "mc")
            row = add_distribution_stats(row, boot, "boot")
            records.append(row)
            print(f"[main]   {contracts}ctr: London={eval_london['strategy_key']} | NY={eval_ny['strategy_key']} | raw_weekly=${sim['avg_payout_per_week']:.0f} | eval_days={sim['eval_pass_days']:.1f} | pass_rate={sim['pass_rate']:.1f}%")

    out_df = pd.DataFrame(records)
    out_csv = OUT_CSV
    if objective != "all":
        out_csv = _PROJECT_ROOT / f"standard_robustness_contract_sweep_report_{spec_name}_{objective}.csv"
    out_df.to_csv(out_csv, index=False)
    print(f"\n[main] report saved to {out_csv}")
    print(out_df[["account_spec", "objective", "contracts", "london_strategy", "ny_strategy", "raw_weekly_payout", "boot_weekly_payout_mean", "raw_eval_pass_days", "boot_eval_pass_days_mean", "raw_pass_rate"]].to_string(index=False))


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--account",
        choices=ACCOUNT_SPECS,
        default=None,
        help="Run only one account spec (default: both)",
    )
    parser.add_argument(
        "--objective",
        choices=OBJECTIVES + ["all"],
        default="all",
        help="Run only one objective (default: all)",
    )
    args = parser.parse_args()
    if args.account:
        ACCOUNT_SPECS = [args.account]
    main(objective=args.objective)