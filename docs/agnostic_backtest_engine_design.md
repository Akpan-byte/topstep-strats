# Design: Language-Agnostic High-Performance Backtest Engine

## Goal
A single backtest engine that can replay any historical market data and execute strategies written in **Python, Rust, C++, or C** with minimal overhead. The engine is responsible for data ingestion, bar/tick replay, order matching, risk rules, PnL accounting, and metrics. Strategies are pure plugins that only decide when to enter/exit.

## Why This Is The Fastest Practical Architecture

- **Engine core in Rust** gives C++-level single-threaded performance with memory safety.
- **Zero-copy data feed** via memory-mapped Parquet/Arrow avoids parsing overhead.
- **C ABI strategy interface** has near-zero call overhead (nanoseconds to microseconds per bar).
- **GPU/FPGA** are faster only for embarrassingly parallel workloads (e.g., vectorized indicator sweeps). For arbitrary strategy state machines, CPU Rust/C++ is the practical fastest because branches and state are hard to parallelize on GPU.

## Historical Data Support

Any OHLCV or tick data can be inserted as long as it normalizes to the engine's canonical schema:

```
timestamp (ns since epoch, UTC)
open, high, low, close (float64)
volume (uint64)
optional: bid, ask, tick_volume, interpolated_flag
```

Supported sources/formats:
- Parquet (preferred — memory-mappable, columnar)
- CSV/JSONL (ingested once to Parquet)
- Real-time feeds via TCP/WebSocket replay

The engine loads data as an Arrow record batch and replays it bar-by-bar or tick-by-tick without copying into strategy memory.

## Core Components

### 1. Data Layer (`data/`) — Rust
- `MarketData::from_parquet(path)` → memory-mapped Arrow table.
- `ReplayIter` yields `Bar` or `Tick` structs in chronological order.
- Session/timezone handling done once at load time; replay uses integer timestamps.

### 2. Strategy Plugin Interface — C ABI

```c
// Opaque strategy instance
typedef void* StrategyHandle;

// Engine calls this once per bar/tick
// strategy writes 0 or 1 signal to `out`
typedef void (*StrategyOnBar)(
    StrategyHandle handle,
    const Bar* bar,
    Signal* out
);

typedef StrategyHandle (*StrategyCreate)(const char* config_json);
typedef void (*StrategyDestroy)(StrategyHandle handle);
```

A `Signal` contains:
- direction (+1 long, -1 short, 0 flat)
- entry_price (0 = market)
- stop_loss, take_profit (optional)
- meta string (JSON for strategy-specific notes)

### 3. Execution Engine (`engine/`) — Rust
- Maintains account state: cash, positions, open orders, equity curve.
- Applies order-matching logic: market/limit/stop, slippage model, commission.
- Enforces risk rules: daily loss limit, trailing drawdown, max positions.
- Calls strategy callback each bar; processes signal; updates PnL.

### 4. Python Shim (`pyo3/`)
- `AgnosticBacktest` Python class.
- Accepts a Python object with `on_bar(self, bar) -> Optional[Signal]`.
- Internally compiles the Python object into a C ABI-compatible adapter using PyO3.

### 5. Metrics & Reporting (`metrics/`) — Rust + Python bindings
- Standard quant suite: Sharpe, Sortino, max drawdown, win rate, profit factor, Calmar, PSR.
- Optional: bootstrap confidence intervals, Monte Carlo (can run on engine output).
- Outputs: CSV trade log + JSON summary + equity curve Parquet.

## Data Flow

```
Parquet/CSV data
      ↓
Arrow record batch (memory-mapped)
      ↓
ReplayIter yields Bar structs
      ↓
Engine → StrategyOnBar callback
      ↓
Strategy returns Signal
      ↓
Engine matches order, updates PnL
      ↓
Trade log + summary metrics
```

## Performance Targets

- Replay: **> 1M bars/second** for simple strategies on a single core.
- Strategy callback overhead: **< 1 µs** for Rust/C++ plugins, **< 10 µs** for Python plugins (PyO3/ctypes).
- Memory: data is memory-mapped; only trade state is allocated per strategy.

## Language Bindings

| Strategy language | Binding mechanism | Overhead |
|-------------------|-------------------|----------|
| Rust              | Static/dynamic link, C ABI | ~ns |
| C / C++           | Dynamic library, C ABI | ~ns |
| Python            | PyO3 / ctypes adapter | ~1–10 µs |
| Future: Go/Zig    | C ABI via cgo / extern | ~ns–µs |

## Why Not GPU/FPGA?

- **GPU**: excellent for vectorized math across many parameter combinations, but terrible for branching state machines (FVG tracking, swing detection, session logic). Data transfer between CPU and GPU dominates for single-strategy replay.
- **FPGA**: fastest for fixed ultra-HFT logic, but strategies must be synthesized to hardware. Not practical for research where strategies change daily.
- **Rust/C++ CPU** is the sweet spot: fast, flexible, and easy to iterate.

## Implementation Phases

1. **MVP** (1–2 days): Rust engine + C ABI + Python shim, OHLCV bar replay, market orders, basic metrics.
2. **Risk rules** (1 day): daily loss limit, trailing drawdown, position sizing.
3. **Strategy library** (2–3 days): port Casper Inverted FVG, ICT Silver Bullet, Velez Elephant Bar to Rust/C ABI; keep Python versions via shim.
4. **Optimization** (2 days): memory-mapped Arrow, SIMD indicator calculations, optional multithreaded parameter sweeps.
5. **Validation harness** (1 day): deterministic replay tests, lookahead-bias checks, parity tests against existing pandas results.

## File Layout (proposed)

```
engine/
  Cargo.toml
  src/
    data.rs       # Parquet/Arrow loading, replay
    strategy.rs   # C ABI trait + plugin loader
    engine.rs     # account, matching, risk
    metrics.rs    # quant metric calculations
    pyo3.rs       # Python bindings
  python/
    agnostic_backtest/
      __init__.py
      adapter.py
  strategies/
    casper_inverted_fvg.rs
    ict_silver_bullet.rs
    casper_inverted_fvg.py   # Python version via shim
```

## Conclusion

This engine is the fastest practical architecture for language-agnostic strategy backtesting. It is bound by CPU single-thread performance and memory bandwidth, both of which Rust/C++ maximize. Any further speed gains would require sacrificing flexibility (GPU/FPGA) or adding massive complexity.
