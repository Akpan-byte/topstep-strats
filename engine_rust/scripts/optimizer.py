# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Created engine_rust/scripts/optimizer.py.
#   - Reads a sweep-results CSV and searches for portfolios of 2-5 strategy
#     legs that meet combined targets (avg/week >= $5,000, win rate >= 80%)
#     while keeping drawdown reasonable.
#   - Uses a greedy forward-selection heuristic with diversity scoring; no
#     brute-force combinatorial explosion.
# WHY: Quickly surface uncorrelated strategy combinations from the existing
#      Paper-1 sweep without running a full portfolio backtest for every subset.

#!/usr/bin/env python3
"""Greedy portfolio optimizer for Paper-1 sweep results."""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import pandas as pd


# Columns required in the input CSV.  Extra columns are ignored.
REQUIRED_COLUMNS = {
    "strategy_id",
    "instrument",
    "session",
    "tp",
    "sl",
    "mode",
    "win_rate",
    "avg_per_week",
    "n_trades",
}


@dataclass
class Leg:
    """One row of the sweep CSV, representing a single strategy leg."""

    strategy_id: str
    instrument: str
    session: str
    tp: float
    sl: float
    mode: str
    win_rate: float
    avg_per_week: float
    n_trades: int
    max_drawdown: float = 0.0
    total_pnl_dollars: float = 0.0

    def key(self) -> Tuple[str, ...]:
        return (self.strategy_id, self.instrument, self.session, str(self.tp), str(self.sl), self.mode)

    def __hash__(self) -> int:
        return hash(self.key())


@dataclass
class Portfolio:
    """A candidate portfolio of strategy legs."""

    legs: List[Leg] = field(default_factory=list)
    expected_weekly_pnl: float = 0.0
    blended_win_rate: float = 0.0
    drawdown_estimate: float = 0.0
    n_trades: int = 0
    diversity_score: float = 0.0
    score: float = 0.0
    meets_targets: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return {
            "n_legs": len(self.legs),
            "expected_weekly_pnl": self.expected_weekly_pnl,
            "blended_win_rate": self.blended_win_rate,
            "drawdown_estimate": self.drawdown_estimate,
            "n_trades": self.n_trades,
            "diversity_score": self.diversity_score,
            "score": self.score,
            "meets_targets": self.meets_targets,
            "legs": " | ".join(
                f"{l.strategy_id}/{l.instrument}/{l.session}/tp{l.tp}/sl{l.sl}/{l.mode}"
                for l in self.legs
            ),
        }


def _load_legs(csv_path: str) -> List[Leg]:
    """Load and deduplicate strategy legs from a sweep CSV."""
    df = pd.read_csv(csv_path)
    missing = REQUIRED_COLUMNS - set(df.columns)
    if missing:
        raise ValueError(f"CSV missing required columns: {sorted(missing)}")

    # Normalize types.
    df = df.copy()
    df["strategy_id"] = df["strategy_id"].astype(str)
    df["instrument"] = df["instrument"].astype(str).str.upper()
    df["session"] = df["session"].astype(str).str.capitalize()
    df["mode"] = df["mode"].astype(str)
    df["tp"] = pd.to_numeric(df["tp"], errors="coerce")
    df["sl"] = pd.to_numeric(df["sl"], errors="coerce")
    df["win_rate"] = pd.to_numeric(df["win_rate"], errors="coerce")
    df["avg_per_week"] = pd.to_numeric(df["avg_per_week"], errors="coerce")
    df["n_trades"] = pd.to_numeric(df["n_trades"], errors="coerce").fillna(0).astype(int)

    # Deduplicate: keep the first row for each unique strategy leg.
    df = df.drop_duplicates(subset=["strategy_id", "instrument", "session", "tp", "sl", "mode"])

    legs: List[Leg] = []
    for _, row in df.iterrows():
        legs.append(
            Leg(
                strategy_id=row["strategy_id"],
                instrument=row["instrument"],
                session=row["session"],
                tp=float(row["tp"]),
                sl=float(row["sl"]),
                mode=row["mode"],
                win_rate=float(row["win_rate"]),
                avg_per_week=float(row["avg_per_week"]),
                n_trades=int(row["n_trades"]),
                max_drawdown=float(row.get("max_drawdown", 0.0)),
                total_pnl_dollars=float(row.get("total_pnl_dollars", 0.0)),
            )
        )
    return legs


def _diversity_score(legs: Sequence[Leg]) -> float:
    """Reward portfolios with varied sessions, instruments, strategy IDs and modes."""
    sessions = {l.session for l in legs}
    instruments = {l.instrument for l in legs}
    sids = {l.strategy_id for l in legs}
    modes = {l.mode for l in legs}
    # Normalize to roughly [0, 1]; more unique dimensions -> higher score.
    return (
        len(sessions) / max(1, len(legs))
        + len(instruments) / max(1, len(legs))
        + len(sids) / max(1, len(legs))
        + len(modes) / max(1, len(legs))
    ) / 4.0


def _evaluate_portfolio(legs: Sequence[Leg], starting_cash: float = 100_000.0) -> Portfolio:
    """Compute combined metrics for a set of legs."""
    total_weekly = sum(l.avg_per_week for l in legs)
    total_trades = sum(l.n_trades for l in legs)
    blended_wr = (
        sum(l.win_rate * l.n_trades for l in legs) / total_trades if total_trades else 0.0
    )

    # Drawdown estimate: sum individual dollar drawdowns, then apply a rough
    # diversification discount based on how many independent sessions/instruments
    # the portfolio spans.
    dd_dollars = sum(abs(l.max_drawdown) * starting_cash for l in legs)
    div = _diversity_score(legs)
    if div > 0:
        dd_estimate = dd_dollars / (1.0 + div * (len(legs) - 1) ** 0.5)
    else:
        dd_estimate = dd_dollars

    portfolio = Portfolio(
        legs=list(legs),
        expected_weekly_pnl=total_weekly,
        blended_win_rate=blended_wr,
        drawdown_estimate=dd_estimate,
        n_trades=total_trades,
        diversity_score=div,
    )

    # Objective: high weekly PnL, high win rate, reasonable drawdown, diverse.
    # Penalize heavy drawdown exposure and reward meeting the hard targets.
    wr_penalty = max(0.0, 0.80 - blended_wr) * 50_000.0
    pnl_penalty = max(0.0, 5_000.0 - total_weekly)
    dd_penalty = max(0.0, dd_estimate - 5_000.0) * 0.5
    portfolio.score = total_weekly - wr_penalty - pnl_penalty - dd_penalty + div * 1_000.0
    return portfolio


def _meets_targets(portfolio: Portfolio, min_weekly: float, min_win_rate: float) -> bool:
    """Return True if the portfolio satisfies the user-defined targets."""
    return (
        portfolio.expected_weekly_pnl >= min_weekly
        and portfolio.blended_win_rate >= min_win_rate
        and portfolio.drawdown_estimate <= max(10_000.0, portfolio.expected_weekly_pnl * 2.0)
    )


def _greedy_search(
    legs: Sequence[Leg],
    min_weekly: float = 5_000.0,
    min_win_rate: float = 0.80,
    max_size: int = 5,
    n_seeds: int = 200,
) -> Tuple[List[Portfolio], List[Portfolio]]:
    """Greedy forward-selection search with random seeds.

    For each seed leg, greedily add the leg that most improves the objective.
    Returns two lists: portfolios that meet the targets and the top-scoring
    best-effort portfolios (used as a fallback when no target-meeting combo
    exists in the provided CSV).
    """
    valid: List[Portfolio] = []
    best_effort: List[Portfolio] = []
    leg_pool = list(legs)

    # Sort legs by individual quality for deterministic seed ordering.
    leg_pool.sort(key=lambda l: l.avg_per_week * l.win_rate, reverse=True)

    for seed_idx in range(min(n_seeds, len(leg_pool))):
        seed = leg_pool[seed_idx]
        current = [seed]

        for _ in range(1, max_size):
            best_leg: Optional[Leg] = None
            best_score = -np.inf
            for leg in leg_pool:
                if leg in current:
                    continue
                # Avoid picking the exact same (sid, instrument, session, mode)
                # more than once, but allow different TP/SL variants.
                if any(
                    leg.strategy_id == c.strategy_id
                    and leg.instrument == c.instrument
                    and leg.session == c.session
                    and leg.mode == c.mode
                    for c in current
                ):
                    continue
                trial = current + [leg]
                portfolio = _evaluate_portfolio(trial)
                if portfolio.score > best_score:
                    best_score = portfolio.score
                    best_leg = leg

            if best_leg is None:
                break
            current.append(best_leg)

            portfolio = _evaluate_portfolio(current)
            portfolio.meets_targets = _meets_targets(portfolio, min_weekly, min_win_rate)
            best_effort.append(portfolio)
            if portfolio.meets_targets:
                valid.append(portfolio)

    # Also evaluate every seed as a 1-leg portfolio for context.
    for leg in leg_pool[:n_seeds]:
        portfolio = _evaluate_portfolio([leg])
        portfolio.meets_targets = _meets_targets(portfolio, min_weekly, min_win_rate)
        best_effort.append(portfolio)
        if portfolio.meets_targets:
            valid.append(portfolio)

    def _dedupe(portfolios: List[Portfolio]) -> List[Portfolio]:
        seen: set = set()
        unique: List[Portfolio] = []
        for p in sorted(portfolios, key=lambda x: x.score, reverse=True):
            key = tuple(sorted(l.key() for l in p.legs))
            if key not in seen:
                seen.add(key)
                unique.append(p)
        return unique

    return _dedupe(valid), _dedupe(best_effort)


def _local_improvement(
    portfolio: Portfolio,
    legs: Sequence[Leg],
    min_weekly: float,
    min_win_rate: float,
    max_size: int = 5,
) -> Optional[Portfolio]:
    """Try swapping each leg with every other leg to improve the score."""
    best = portfolio
    leg_pool = list(legs)
    for i, _ in enumerate(portfolio.legs):
        for replacement in leg_pool:
            if replacement in portfolio.legs:
                continue
            trial = portfolio.legs[:i] + [replacement] + portfolio.legs[i + 1 :]
            if len(trial) > max_size:
                continue
            p = _evaluate_portfolio(trial)
            if _meets_targets(p, min_weekly, min_win_rate) and p.score > best.score:
                best = p
    return best if best.score > portfolio.score else None


def optimize(
    csv_path: str,
    min_weekly: float = 5_000.0,
    min_win_rate: float = 0.80,
    max_size: int = 5,
    n_seeds: int = 200,
    local_search: bool = True,
    top_n: int = 20,
) -> Tuple[List[Portfolio], List[Leg], bool]:
    """Run the optimizer and return ranked candidate portfolios plus all legs.

    The third return value is True if the returned candidates all meet the
    hard targets; False means they are the best-effort portfolios found.
    """
    legs = _load_legs(csv_path)

    # Filter to legs that are individually profitable; losers rarely help.
    legs = [l for l in legs if l.avg_per_week > 0 and l.n_trades > 0]

    valid, best_effort = _greedy_search(legs, min_weekly, min_win_rate, max_size, n_seeds)
    meets_targets = True

    if valid:
        candidates = valid
    else:
        candidates = best_effort
        meets_targets = False

    if local_search and candidates:
        improved: List[Portfolio] = []
        search_pool = valid if valid else best_effort
        for p in search_pool[:top_n * 2]:
            better = _local_improvement(p, legs, min_weekly, min_win_rate, max_size)
            improved.append(better if better is not None else p)
        # Re-rank and deduplicate.
        seen: set = set()
        unique: List[Portfolio] = []
        for p in sorted(improved, key=lambda x: x.score, reverse=True):
            key = tuple(sorted(l.key() for l in p.legs))
            if key not in seen:
                seen.add(key)
                unique.append(p)
        candidates = unique if unique else candidates

    return candidates[:top_n], legs, meets_targets


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Greedy portfolio optimizer for Paper-1 sweep results.")
    parser.add_argument("csv", help="Path to sweep results CSV")
    parser.add_argument("--min-weekly", type=float, default=5_000.0, help="Minimum combined avg/week ($)")
    parser.add_argument("--min-win-rate", type=float, default=0.80, help="Minimum blended win rate (0-1)")
    parser.add_argument("--max-size", type=int, default=5, help="Maximum number of legs per portfolio")
    parser.add_argument("--n-seeds", type=int, default=200, help="Number of greedy seed legs")
    parser.add_argument("--no-local-search", action="store_true", help="Disable local-search improvement")
    parser.add_argument("--top-n", type=int, default=5, help="Number of portfolios to print")
    parser.add_argument("--output", default=None, help="Optional JSON/CSV output path")
    args = parser.parse_args(argv)

    candidates, legs, meets_targets = optimize(
        args.csv,
        min_weekly=args.min_weekly,
        min_win_rate=args.min_win_rate,
        max_size=args.max_size,
        n_seeds=args.n_seeds,
        local_search=not args.no_local_search,
        top_n=args.top_n,
    )

    print(f"Loaded {len(legs)} unique strategy legs from {args.csv}")
    if not meets_targets:
        print(
            f"No portfolios met the hard targets (weekly>=${args.min_weekly:,.0f}, "
            f"win_rate>={args.min_win_rate:.0%}); showing best-effort candidates instead."
        )
    print(f"Top {args.top_n} candidate portfolios (min_weekly=${args.min_weekly:,.0f}, min_win_rate={args.min_win_rate:.0%}):")
    for i, p in enumerate(candidates[: args.top_n], 1):
        target_flag = "✓" if p.meets_targets else "✗"
        print(
            f"  {i}. [{target_flag}] score={p.score:,.0f}  weekly=${p.expected_weekly_pnl:,.0f}  "
            f"win_rate={p.blended_win_rate:.1%}  dd_est=${p.drawdown_estimate:,.0f}  "
            f"n_trades={p.n_trades}  diversity={p.diversity_score:.2f}  legs={len(p.legs)}"
        )
        print(f"     {p.to_dict()['legs']}")

    if args.output:
        out_path = Path(args.output)
        rows = [p.to_dict() for p in candidates[: args.top_n]]
        df = pd.DataFrame(rows)
        if out_path.suffix.lower() == ".json":
            df.to_json(out_path, orient="records", indent=2)
        else:
            df.to_csv(out_path, index=False)
        print(f"Wrote top {len(rows)} portfolios to {out_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
