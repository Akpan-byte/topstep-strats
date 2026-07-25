<!-- CHANGE_SUMMARY
2026-07-25  kilo
  - Added README.md covering project purpose, setup, local smoke test, and
    GitHub Actions parallel orchestration.
WHY: Future agents and CI need a single landing page for the project.
-->

# TopStep Futures Strategies

Parallel backtesting framework for two futures trading strategies on 10 years of
1-minute NQ data.

## Strategies

- **Kasen ORB** (`topstep_strats/strategies/kasen_orb.py`) — opening-range-breakout
  strategy that marks the first 15-minute NY session candle, waits for a break,
  and enters on a retest of the range midpoint.
- **Nitro CRT** (`topstep_strats/strategies/nitro_crt.py`) — wick-sweep / CRT
  strategy that trades reversals after a higher-timeframe level is swept.

## Repository layout

```
topstep_strats/
  data.py                # Loader, resampler, session mask, date splitter
  backtest.py            # Trade execution, equity curve, drawdowns, costs
  metrics.py             # PSR, DSR, MC/bootstrap, Bayesian Sharpe, etc.
  strategies/
    kasen_orb.py
    nitro_crt.py
scripts/
  run_chunk.py           # One strategy × one date chunk entry point
  aggregate.py           # Combines all chunk JSONs into final report + chart
  run_local.py           # Local smoke test across a small date range
.github/workflows/
  topstep_parallel.yml   # 20 chunks × 2 strategies = 40 parallel jobs
tests/                   # Pytest suite
data -> /tmp/market_data # Symlink to the NQ CSV
```

## Quick start

Create the virtual environment and install dependencies:

```bash
cd /tmp/topstep-strats
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

The data file is expected at `/tmp/market_data/NQ_1min.csv`. The symlink
`topstep-strats/data` points to that directory.

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
  --strategy kasen_orb \
  --start-date 2024-01-01 \
  --end-date 2024-01-31 \
  --output results/kasen_2024_01.json
```

Optional `--params` accepts a JSON dict with `strategy_params`,
`backtest_params`, and `metrics_kwargs`.

## GitHub Actions workflow

`.github/workflows/topstep_parallel.yml` runs the full backtest on every push to
`main` or via `workflow_dispatch`.

- **Chunk jobs**: 20 date chunks × 2 strategies = 40 parallel runners. Each job
  downloads the market data via `rclone` from `akpanbrain:data/market_data/NQ_1min.csv`
  (with a configurable HTTP fallback URL), runs `scripts/run_chunk.py`, and
  uploads a small JSON artifact.
- **Aggregate job**: Downloads all artifacts, runs `scripts/aggregate.py`, and
  uploads the combined `final_report/` artifact containing `final_report.json`
  and `equity_by_strategy.png`.

### Required repository secrets

| Secret | Purpose |
|--------|---------|
| `RCLONE_CONFIG_AKPANBRAIN_TYPE` | rclone remote type (e.g. `s3`) |
| `RCLONE_CONFIG_AKPANBRAIN_PROVIDER` | Cloud provider, if required |
| `RCLONE_CONFIG_AKPANBRAIN_ACCESS_KEY_ID` | Access key / username |
| `RCLONE_CONFIG_AKPANBRAIN_SECRET_ACCESS_KEY` | Secret key / password |
| `RCLONE_CONFIG_AKPANBRAIN_ENDPOINT` | Custom endpoint, if required |
| `RCLONE_CONFIG_AKPANBRAIN_REGION` | Region, if required |
| `MARKET_DATA_URL` | Fallback HTTP(S) URL for the CSV |

## Tests

Run the targeted test suite:

```bash
python -m pytest tests/test_github_actions.py -v
```

Do not run the full 10-year backtest locally; it is intended for the parallel CI
pipeline.
