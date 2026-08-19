# CHANGE_SUMMARY
# 2026-08-19  kilo
#   - Replaced the Kasen-ORB/Nitro-CRT grid with a thin wrapper around
#     topstep_strats.strategies.paper1_matrix, which holds the exact 100-row
#     Paper-1 strategy matrix from the user's research report.
#   - Keeps INSTRUMENT_CONFIG, SESSION_CONFIG, CHUNKS, default_data_path(),
#     and make_strategy_key() so runners and the GitHub Actions workflow
#     continue to import a single canonical source.
# WHY: The Paper-1 sweep must evaluate the 7 blueprints (ICT Silver Bullet,
#      Casper SMC Inverted FVG, Velez 20/200 Elephant Bar, Rosato S/D Absorption,
#      Carter TTM Squeeze, Raschke Holy Grail, Wade PATs Second Entry), not the
#      legacy Kasen/Nitro strategies.

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Iterable, List, Tuple

from topstep_strats.strategies.paper1_matrix import (
    get_strategy_config as _matrix_get_strategy_config,
    list_strategy_ids as _matrix_list_strategy_ids,
)

N_STRATEGIES = 100

INSTRUMENT_CONFIG: Dict[str, Dict[str, Any]] = {
    "NQ": {"tick_size": 0.25, "point_value": 20.0},
    "ES": {"tick_size": 0.25, "point_value": 50.0},
    "YM": {"tick_size": 1.0, "point_value": 5.0},
}

SESSION_CONFIG: Dict[str, Dict[str, str]] = {
    "Asian": {"start": "20:00", "end": "23:00", "tz": "America/New_York"},
    "London": {"start": "03:00", "end": "11:00", "tz": "America/New_York"},
    "NY": {"start": "09:30", "end": "16:00", "tz": "America/New_York"},
}

# 20 six-month chunks covering the available 1-minute data (2016-06-01 to 2026-05-29).
CHUNKS: List[Tuple[str, str]] = [
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


def get_strategy_config(strategy_id: int) -> Dict[str, Any]:
    """Return the Paper-1 configuration for a zero-based strategy ID.

    IDs 0-99 map to matrix rows '001'-'100'.
    """
    if not isinstance(strategy_id, int) or strategy_id < 0 or strategy_id >= N_STRATEGIES:
        raise ValueError(f"strategy_id must be an integer in [0, {N_STRATEGIES}), got {strategy_id!r}")
    matrix_id = f"{strategy_id + 1:03d}"
    return _matrix_get_strategy_config(matrix_id)


def iter_strategy_ids(start: int = 0, end: int | None = None) -> Iterable[int]:
    """Yield strategy IDs in the requested half-open range [start, end)."""
    if end is None:
        end = N_STRATEGIES
    if start < 0 or end > N_STRATEGIES or start >= end:
        raise ValueError(f"invalid range [{start}, {end}) for {N_STRATEGIES} strategies")
    yield from range(start, end)


_PROJECT_ROOT = Path(__file__).resolve().parent.parent


def default_data_path(instrument: str) -> str:
    """Return the first available default data file for an instrument."""
    root = _PROJECT_ROOT / "data"
    csv = root / f"{instrument}_1min.csv"
    if csv.exists():
        return str(csv)
    parquet = root / f"{instrument}_1min.parquet"
    if parquet.exists():
        return str(parquet)
    return f"/tmp/market_data/{instrument}_1min.csv"


def make_strategy_key(strategy_id: int, instrument: str, session: str) -> str:
    """Composite key used for aggregation grouping."""
    cfg = get_strategy_config(strategy_id)
    blueprint = cfg.get("blueprint", "unknown")
    return f"paper1_{strategy_id:03d}_{blueprint}_{instrument}_{session}"
