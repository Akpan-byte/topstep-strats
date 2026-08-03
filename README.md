<!-- CHANGE_SUMMARY
2026-07-25  kilo
  - Original README for the TopStep parallel backtest project.
2026-07-25  kilo
  - Updated to reflect multi-instrument support (NQ, ES, YM), shared-account
    portfolio analysis, and the Nitro CRT strategy selection.
2026-08-03  coder
  - Documented the HTF/target sweep: 48 config-jobs on GitHub Actions (NQ+ES)
    + the YM slice on the Akpan laptop, zstd parquet data (data-v2.0 release),
    vectorized strategy, overlapping 7-day-warmup chunks, and by_config /
    sweep_summary aggregation.
WHY: The strategy evolved from a single-instrument NQ backtest to a portfolio
     of futures tested under Topstep rules, and the sweep must settle which
     timeframe + target mode is actually real.
-->

# TopStep Futures Strategies

Parallel backtesting framework for futures trading strategies on 10 years of
1-minute data for **NQ, ES, and YM**.

## Strategy selection

| Strategy | Source | Status |
|----------|--------|--------|
| **Nitro CRT** | Instagram CRT/reversal reels | **Selected** — robust edge across NQ/ES/YM and Topstep rules |
| Kasen ORB | Instagram ORB reels | Discarded — skipped too many trades under Topstep rules |

Nitro CRT is a higher-timeframe sweep + lower-timeframe reversal strategy.  The
previous higher-timeframe candle's high/low are used as CRT levels.  A 1-minute
sweep and close back through the level triggers entry (next bar's open — no
look-ahead), with a fixed 1.5:1 reward-to-risk target, or the opposite side of
the HTF level depending on `target_mode`.  The full HTF/target sweep decides
which timeframe (5m/15m/30m/1h/2h/4h) and target mode (fixed_rr/opposite) is
actually real vs a simulation artifact.

Full findings are documented in:
`/config/topstep-strats-analysis/docs/FINDINGS.md`

## Repository layout

```
topstep_strats/
  data.py                # Loader, resampler, session mask, date splitter
  backtest.py            # Trade execution, equity curve, drawdowns, costs
  metrics.py             # PSR, DSR, MC/bootstrap, Bayesian Sharpe, etc.
  strategies/
    kasen_orb.py         # Opening-range-breakout strategy (discarded)
    nitro_crt.py         # Selected CRT sweep/reversal strategy
scripts/
  run_chunk.py           # One strategy × one date chunk entry point
  run_sweep_job.py       # One sweep config × 20 overlapping chunks (GH job)
  run_parallel_laptop.py # YM sweep slice on the Akpan laptop (16 cores) + upload
  aggregate.py           # Combines all chunk JSONs into final report + chart
  run_local.py           # Local smoke test across a small date range
.github/workflows/
  topstep_parallel.yml   # HTF/target sweep: 48 config-jobs on 20 runners
/config/topstep-strats-analysis/scripts/  # Analysis scripts for sizing & metrics
tests/                   # Pytest suite
```

## HTF/target sweep

The sweep tests every combination of `instrument {NQ, ES, YM}` ×
`htf_timeframe {5m, 15m, 30m, 1h, 2h, 4h}` × `target_mode {fixed_rr, opposite}`
× `scenario {first_only, reentries}` = 72 configs over the full 10-year data.

- **GitHub Actions** runs the NQ+ES slice (48 config-jobs) on all 20 parallel
  runners — ~3 waves. Each job downloads the zstd-compressed parquet once and
  loops the 20 overlapping chunks in-process.
- **Akpan laptop** runs the YM slice (24 configs) with 16 cores via
  `run_parallel_laptop.py`, then uploads `laptop_results.tar.gz` to the
  `data-v2.0` release; the aggregate job merges it automatically.
- **Overlapping chunks**: each chunk loads 7 warmup days before its start so
  HTF CRT levels have context; signals outside the chunk range are dropped, so
  no trade is skipped or double-counted at boundaries.
- **Compressed + vectorized**: data is zstd parquet (~40MB/instrument vs
  ~190MB CSV), and `nitro_crt._simulate_trade` is numpy-vectorized
  (searchsorted + argmax), cutting per-chunk signal time ~6x (1h: 12s → 2s).
- **Aggregation**: `aggregate.py` emits `by_config`
  (`instrument__htf__target__scenario`) plus a `sweep_summary` ranked by net
  points, so the real config is identifiable at a glance.

## Quick start

Create the virtual environment and install dependencies:

```bash
cd /config/topstep-strats-analysis/strategy_code
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Data files are expected at `/tmp/mktdata/{NQ,ES,YM}_1min.parquet` (zstd,
preferred, from the `data-v2.0` release) or `/tmp/market_data/{NQ,ES,YM}_1min.csv`
as fallback.

## Local smoke test

Run a small slice of the pipeline locally without uploading artifacts:

```bash
source venv/bin/activate
python scripts/run_local.py --start-date 2024-01-01 --end-date 2024-03-31 --n-chunks 2
```

This produces JSON results and an aggregate report under `local_results/`.

## Running a single chunk

```bash
python scripts/run_chunk.py \
  --strategy nitro_crt \
  --start-date 2024-01-01 \
  --end-date 2024-01-31 \
  --output results/nitro_2024_01.json
```

Optional `--params` accepts a JSON dict with `strategy_params`,
`backtest_params`, and `metrics_kwargs`. Sweep-specific `--htf` and
`--target-mode` flags override the strategy params so the matrix can pivot on
those two dimensions without editing JSON.

## GitHub Actions workflow

`.github/workflows/topstep_parallel.yml` runs the HTF/target sweep on every push
to `main` or via `workflow_dispatch`.

- **Sweep jobs**: matrix `instrument {NQ, ES} × htf {5m,15m,30m,1h,2h,4h} ×
  target {fixed_rr, opposite} × scenario {first_only, reentries}` = 48
  config-jobs on 20 parallel runners. Each downloads the matching parquet from
  the `data-v2.0` release and runs `scripts/run_sweep_job.py` (all 20
  overlapping chunks in-process).
- **Aggregate job**: Downloads all `config-*` artifacts, merges the laptop's
  `laptop_results.tar.gz` from the release when present (YM slice), runs
  `scripts/aggregate.py`, and uploads the combined `final_report/` artifact.

## TopStep sizing recommendations

From the 10-year backtest:

- **$50k account → NQ+YM @ 2 contracts** (avg weekly profit ~$3,764, never blown).
- **$100k account → NQ+YM @ 5 contracts** (avg weekly profit ~$9,397, never blown).
- **$150k account → NQ+ES+YM @ 4 contracts** (avg weekly profit ~$10,113), but
  note this combination blew the account in the 2025-26 regime at higher sizing.

See `docs/FINDINGS.md` for the full quant-suite metrics, regime-split test, and
Monte-Carlo / bootstrap confidence intervals.

## Tests

Run the targeted test suite:

```bash
python -m pytest tests/test_github_actions.py -v
```

Do not run the full 10-year backtest locally; it is intended for the parallel CI
pipeline or the Akpan laptop multi-core run.
