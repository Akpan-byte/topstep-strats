# CHANGE_SUMMARY
# 2026-08-20  kilo
#   - Temporary profiling script to measure Paper-1 signal generation and
#     runner exit simulation times before/after optimization.
# WHY: Establish baseline per-blueprint timings and identify the bottleneck.

import time
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_PROJECT_ROOT))

from topstep_strats.data import load_market_data, get_session_mask, split_by_date
from topstep_strats.strategies.paper1_matrix import generate_signals, get_strategy_config, _atr
from scripts.runner_exit_numba import apply_runner_to_signals


def main():
    print("Loading NQ...")
    t0 = time.perf_counter()
    df = load_market_data(_PROJECT_ROOT / "data" / "NQ_1min.parquet")
    df = df.copy()
    df["atr"] = _atr(df, 14)
    print(f"Load: {time.perf_counter()-t0:.2f}s, rows={len(df)}")

    mask = get_session_mask(df, "09:30", "16:00", "America/New_York")
    df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
    print(f"Session rows={len(df_s)}")

    sids = ["001", "007", "013", "020", "027", "035", "043"]
    for sid in sids:
        cfg = get_strategy_config(sid)
        cfg.update({
            "instrument": "NQ", "tick_size": 0.25, "point_value": 20.0,
            "session": "NY", "session_start": "09:30", "session_end": "16:00",
            "tp_atr": 2.0, "sl_atr": 5.0,
            "session_only": True, "one_trade_per_day": True, "stop_first": True,
        })
        t0 = time.perf_counter()
        signals = generate_signals(df_s, cfg)
        t1 = time.perf_counter()
        n = len(signals)
        out = f"  {sid} ({cfg['blueprint'][:22]:22s}) signals={n:4d} gen={t1-t0:.3f}s"
        if not signals.empty:
            t2 = time.perf_counter()
            apply_runner_to_signals(df, signals, mode="trail", trail_atr_mult=0.5)
            t3 = time.perf_counter()
            out += f" runner1={t3-t2:.3f}s"
        print(out)


if __name__ == "__main__":
    main()
