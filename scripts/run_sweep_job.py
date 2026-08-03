# CHANGE_SUMMARY
# 2026-08-03  coder
#   - Created run_sweep_job.py: runs all 20 overlapping date chunks for ONE
#     sweep configuration (instrument x htf x target_mode x scenario) in a
#     single process, reusing run_chunk.py's entry point for each chunk.
#   - Each chunk loads 7 warmup days of leading data so HTF CRT levels have
#     context; run_chunk drops signals outside the chunk range so adjacent
#     chunks never double-count a trade (overlap, not duplication).
#   - Writes one JSON artifact per chunk into a per-config output directory,
#     so the aggregate step consumes the same file format as the old per-chunk
#     matrix (aggregate.py is unchanged).
# WHY: The full HTF/target sweep is 6 HTFs x 2 targets x 2 scenarios x 3
#      instruments = 72 configs. Running one config per job (72 jobs across
#      20 GitHub Actions runners, ~4 waves) downloads the parquet once per
#      config instead of once per chunk, and lets the laptop take a
#      complementary instrument slice with the same artifact format.
#      Overlapping chunks guarantee no trade is skipped at chunk boundaries.

import argparse
import json
import sys
import time
from pathlib import Path

# Make scripts/ importable so we can reuse run_chunk.py's main().
sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_chunk import main as run_chunk_main  # noqa: E402

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

HTFS = ["5m", "15m", "30m", "1h", "2h", "4h"]
TARGET_MODES = ["fixed_rr", "opposite"]
SCENARIOS = ["first_only", "reentries"]
INSTRUMENTS = ["NQ", "ES", "YM"]

BASE_PARAMS = {
    "strategy_params": {
        "htf_timeframe": "1h",  # overridden per job
        "entry_timeframe": "1m",
        "risk_reward": 1.5,
        "buffer_ticks": 1,
        "target_mode": "fixed_rr",  # overridden per job
        "session_only": True,
        "session_start": "09:30",
        "session_end": "16:00",
        "tz": "America/New_York",
        "entry_mode": "next_open",
    }
}


def run_config(instrument, htf, target_mode, scenario, data_path, output_dir, warmup_days=7):
    """Run one config across all 20 chunks. Returns (ok_count, total_time_s)."""
    out_dir = Path(output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    params = json.loads(json.dumps(BASE_PARAMS))
    params["strategy_params"]["htf_timeframe"] = htf
    params["strategy_params"]["target_mode"] = target_mode

    t0 = time.time()
    ok = 0
    for start, end in CHUNKS:
        out_file = out_dir / f"result_{instrument}_{htf}_{target_mode}_{scenario}_{start}_{end}.json"
        run_chunk_main([
            "--strategy", "nitro_crt",
            "--instrument", instrument,
            "--scenario", scenario,
            "--warmup-days", str(warmup_days),
            "--start-date", start,
            "--end-date", end,
            "--data-path", str(data_path),
            "--params", json.dumps(params),
            "--output", str(out_file),
        ])
        ok += 1
    elapsed = time.time() - t0
    return ok, elapsed


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run one sweep config (instrument x htf x target x scenario) over all 20 chunks."
    )
    parser.add_argument("--instrument", required=True, choices=INSTRUMENTS)
    parser.add_argument("--htf", required=True, choices=HTFS)
    parser.add_argument("--target-mode", required=True, choices=TARGET_MODES)
    parser.add_argument("--scenario", required=True, choices=SCENARIOS)
    parser.add_argument("--data-path", required=True, help="Parquet or CSV market data path")
    parser.add_argument("--output-dir", required=True, help="Directory for chunk JSONs")
    parser.add_argument("--warmup-days", type=int, default=7)
    args = parser.parse_args(argv)

    ok, elapsed = run_config(
        args.instrument, args.htf, args.target_mode, args.scenario,
        args.data_path, args.output_dir, args.warmup_days,
    )
    print(f"[run_sweep_job] {args.instrument}/{args.htf}/{args.target_mode}/{args.scenario}: "
          f"{ok}/{len(CHUNKS)} chunks in {elapsed:.1f}s")


if __name__ == "__main__":
    main()
