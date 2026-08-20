// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Created Rust/PyO3 language-agnostic backtest engine MVP.
// 2026-08-20  kilo
//   - Expanded PyO3 bindings: Parquet loader, full engine config, extended metrics,
//     prefilled-trade runner, and Topstep risk config.
// 2026-08-20  kilo
//   - Exposed new combine pass/fail and time-to-pass Summary fields to Python.
// WHY: Fast, memory-safe core that can host strategies written in Rust, C, or Python.

pub mod data;
pub mod engine;
pub mod metrics;
pub mod strategy;

pub use data::{load_csv, load_parquet, Bar, ReplayIter};
pub use engine::{Engine, EngineConfig, Fill, PrefilledTrade, TopstepConfig, Trade};
pub use engine_abi::{Signal, Strategy, StrategyVTable};
pub use metrics::Summary;

use pyo3::prelude::*;

/// Python module entry point.
#[pymodule]
fn _rust(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<pyo3_engine::PyEngine>()?;
    m.add_class::<pyo3_engine::PyBar>()?;
    m.add_class::<pyo3_engine::PySignal>()?;
    m.add_class::<pyo3_engine::PySummary>()?;
    m.add_class::<pyo3_engine::PyTopstepConfig>()?;
    Ok(())
}

/// Python bindings live in a submodule so the core engine stays language-agnostic.
pub mod pyo3_engine {
    use super::{Bar, Engine, EngineConfig, PrefilledTrade, Signal, Strategy, Summary, TopstepConfig};
    use pyo3::{Bound, Py, PyObject, PyResult, Python};
    use pyo3::exceptions::PyIOError;
    use pyo3::types::{PyAnyMethods, PyTuple};
    use pyo3::prelude::{PyAny, PyErr};
    use pyo3::{pyclass, pymethods};

    /// Python-facing OHLCV bar (read-only view).
    #[pyclass(name = "Bar")]
    #[derive(Clone, Debug)]
    pub struct PyBar {
        #[pyo3(get)]
        pub timestamp_ns: i64,
        #[pyo3(get)]
        pub open: f64,
        #[pyo3(get)]
        pub high: f64,
        #[pyo3(get)]
        pub low: f64,
        #[pyo3(get)]
        pub close: f64,
        #[pyo3(get)]
        pub volume: u64,
    }

    impl From<&Bar> for PyBar {
        fn from(b: &Bar) -> Self {
            PyBar {
                timestamp_ns: b.timestamp_ns,
                open: b.open,
                high: b.high,
                low: b.low,
                close: b.close,
                volume: b.volume,
            }
        }
    }

    /// Python-facing signal.
    #[pyclass(name = "Signal")]
    #[derive(Clone, Debug)]
    pub struct PySignal {
        #[pyo3(get)]
        pub direction: i8,
        #[pyo3(get)]
        pub entry_price: f64,
        #[pyo3(get)]
        pub stop_loss: f64,
        #[pyo3(get)]
        pub take_profit: f64,
    }

    impl From<Signal> for PySignal {
        fn from(s: Signal) -> Self {
            PySignal {
                direction: s.direction,
                entry_price: s.entry_price,
                stop_loss: s.stop_loss,
                take_profit: s.take_profit,
            }
        }
    }

    impl From<PySignal> for Signal {
        fn from(s: PySignal) -> Self {
            Signal {
                direction: s.direction,
                entry_price: s.entry_price,
                stop_loss: s.stop_loss,
                take_profit: s.take_profit,
            }
        }
    }

    /// Python-facing summary.
    #[pyclass(name = "Summary")]
    #[derive(Clone, Debug)]
    pub struct PySummary {
        #[pyo3(get)]
        pub total_return: f64,
        #[pyo3(get)]
        pub n_trades: usize,
        #[pyo3(get)]
        pub win_rate: f64,
        #[pyo3(get)]
        pub profit_factor: f64,
        #[pyo3(get)]
        pub max_drawdown: f64,
        #[pyo3(get)]
        pub sharpe: f64,
        #[pyo3(get)]
        pub sortino: f64,
        #[pyo3(get)]
        pub calmar: f64,
        #[pyo3(get)]
        pub avg_trade: f64,
        #[pyo3(get)]
        pub avg_per_day: f64,
        #[pyo3(get)]
        pub avg_per_week: f64,
        #[pyo3(get)]
        pub hold_time_p50: f64,
        #[pyo3(get)]
        pub hold_time_p90: f64,
        #[pyo3(get)]
        pub hold_time_p95: f64,
        #[pyo3(get)]
        pub failures: u32,
        #[pyo3(get)]
        pub final_equity: f64,
        #[pyo3(get)]
        pub total_pnl: f64,
        #[pyo3(get)]
        pub executed_trades: usize,
        #[pyo3(get)]
        pub daily_limit_hits: u32,
        #[pyo3(get)]
        pub trailing_limit_hits: u32,
        #[pyo3(get)]
        pub account_failed: bool,
        #[pyo3(get)]
        pub account_blowups: u32,
        #[pyo3(get)]
        pub profit_target_reached: bool,
        #[pyo3(get)]
        pub passed_combines: u32,
        #[pyo3(get)]
        pub failed_combines: u32,
        #[pyo3(get)]
        pub combine_pass_rate: f64,
        #[pyo3(get)]
        pub avg_days_to_pass: f64,
        #[pyo3(get)]
        pub median_days_to_pass: f64,
        #[pyo3(get)]
        pub min_days_to_pass: u32,
        #[pyo3(get)]
        pub max_days_to_pass: u32,
        #[pyo3(get)]
        pub days_to_pass_p50: f64,
        #[pyo3(get)]
        pub days_to_pass_p75: f64,
        #[pyo3(get)]
        pub days_to_pass_p90: f64,
        #[pyo3(get)]
        pub days_to_pass_p95: f64,
        #[pyo3(get)]
        pub days_to_pass_p99: f64,
        #[pyo3(get)]
        pub avg_trades_to_pass: f64,
        #[pyo3(get)]
        pub median_trades_to_pass: f64,
    }

    impl From<Summary> for PySummary {
        fn from(s: Summary) -> Self {
            PySummary {
                total_return: s.total_return,
                n_trades: s.n_trades,
                win_rate: s.win_rate,
                profit_factor: s.profit_factor,
                max_drawdown: s.max_drawdown,
                sharpe: s.sharpe,
                sortino: s.sortino,
                calmar: s.calmar,
                avg_trade: s.avg_trade,
                avg_per_day: s.avg_per_day,
                avg_per_week: s.avg_per_week,
                hold_time_p50: s.hold_time_p50,
                hold_time_p90: s.hold_time_p90,
                hold_time_p95: s.hold_time_p95,
                failures: s.failures,
                final_equity: s.final_equity,
                total_pnl: s.total_pnl,
                executed_trades: s.executed_trades,
                daily_limit_hits: s.daily_limit_hits,
                trailing_limit_hits: s.trailing_limit_hits,
                account_failed: s.account_failed,
                account_blowups: s.account_blowups,
                profit_target_reached: s.profit_target_reached,
                passed_combines: s.passed_combines,
                failed_combines: s.failed_combines,
                combine_pass_rate: s.combine_pass_rate,
                avg_days_to_pass: s.avg_days_to_pass,
                median_days_to_pass: s.median_days_to_pass,
                min_days_to_pass: s.min_days_to_pass,
                max_days_to_pass: s.max_days_to_pass,
                days_to_pass_p50: s.days_to_pass_p50,
                days_to_pass_p75: s.days_to_pass_p75,
                days_to_pass_p90: s.days_to_pass_p90,
                days_to_pass_p95: s.days_to_pass_p95,
                days_to_pass_p99: s.days_to_pass_p99,
                avg_trades_to_pass: s.avg_trades_to_pass,
                median_trades_to_pass: s.median_trades_to_pass,
            }
        }
    }

    /// Python-facing Topstep-style risk configuration.
    #[pyclass(name = "TopstepConfig", frozen)]
    #[derive(Clone, Debug)]
    pub struct PyTopstepConfig {
        #[pyo3(get)]
        pub enabled: bool,
        #[pyo3(get)]
        pub daily_drawdown_limit: f64,
        #[pyo3(get)]
        pub trailing_drawdown_limit: f64,
        #[pyo3(get)]
        pub profit_target: f64,
        #[pyo3(get)]
        pub reset_on_failure: bool,
    }

    #[pymethods]
    impl PyTopstepConfig {
        #[new]
        #[pyo3(signature = (enabled=false, daily_drawdown_limit=900.0, trailing_drawdown_limit=2_000.0, profit_target=3_000.0, reset_on_failure=false))]
        fn new(
            enabled: bool,
            daily_drawdown_limit: f64,
            trailing_drawdown_limit: f64,
            profit_target: f64,
            reset_on_failure: bool,
        ) -> Self {
            PyTopstepConfig {
                enabled,
                daily_drawdown_limit,
                trailing_drawdown_limit,
                profit_target,
                reset_on_failure,
            }
        }
    }

    /// Adapter that turns a Python object into the engine's Strategy trait.
    struct PyStrategy {
        obj: PyObject,
    }

    impl Strategy for PyStrategy {
        fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
            Python::with_gil(|py| {
                let pybar: Py<PyBar> = match Py::new(py, PyBar::from(bar)) {
                    Ok(obj) => obj,
                    Err(e) => {
                        e.print(py);
                        return None;
                    }
                };
                let args = PyTuple::new_bound(py, &[&pybar]);
                match self.obj.call_method1(py, "on_bar", &args) {
                    Ok(result) => {
                        if result.is_none(py) {
                            None
                        } else {
                            // Accept either the Rust PySignal class or any Python object with
                            // the expected attributes (e.g. the Python shim's Signal dataclass).
                            let bound = result.bind(py);
                            match (
                                bound.getattr("direction").and_then(|a| a.extract::<i8>()),
                                bound.getattr("entry_price").and_then(|a| a.extract::<f64>()),
                                bound.getattr("stop_loss").and_then(|a| a.extract::<f64>()),
                                bound.getattr("take_profit").and_then(|a| a.extract::<f64>()),
                            ) {
                                (Ok(direction), Ok(entry_price), Ok(stop_loss), Ok(take_profit)) => {
                                    Some(Signal {
                                        direction,
                                        entry_price,
                                        stop_loss,
                                        take_profit,
                                    })
                                }
                                _ => {
                                    eprintln!("agnostic_backtest: failed to extract Signal attributes from Python return value");
                                    None
                                }
                            }
                        }
                    }
                    Err(e) => {
                        e.print(py);
                        None
                    }
                }
            })
        }
    }

    /// Python-facing engine.
    #[pyclass(name = "Engine")]
    pub struct PyEngine {
        engine: Engine,
    }

    #[pymethods]
    impl PyEngine {
        #[new]
        #[pyo3(signature = (
            cash=100_000.0,
            commission=0.0,
            slippage=0.0,
            daily_loss_limit=0.0,
            trailing_drawdown=0.0,
            reset_on_failure=true,
            point_value=1.0,
            position_sizing=false,
            timezone=String::new(),
            session_start=None,
            session_end=None
        ))]
        fn new(
            cash: f64,
            commission: f64,
            slippage: f64,
            daily_loss_limit: f64,
            trailing_drawdown: f64,
            reset_on_failure: bool,
            point_value: f64,
            position_sizing: bool,
            timezone: String,
            session_start: Option<String>,
            session_end: Option<String>,
        ) -> Self {
            PyEngine {
                engine: Engine::new(EngineConfig {
                    starting_cash: cash,
                    commission_per_contract: commission,
                    slippage_ticks: slippage,
                    daily_loss_limit,
                    trailing_drawdown,
                    reset_on_failure,
                    point_value,
                    position_sizing,
                    timezone,
                    session_start,
                    session_end,
                }),
            }
        }

        /// Load OHLCV bars from a CSV file (timestamp_ns,open,high,low,close,volume).
        fn load_csv(&self, _path: &str) -> PyResult<()> {
            // Kept for API symmetry; data is loaded inside run_csv below.
            Ok(())
        }

        /// Run a Python strategy object over a CSV file and return a summary.
        fn run_csv(&mut self, path: &str, strategy_obj: &Bound<'_, PyAny>) -> PyResult<PySummary> {
            let bars = crate::data::load_csv(path).map_err(|e| {
                PyErr::new::<PyIOError, _>(format!("CSV load failed: {e}"))
            })?;
            let mut py_strategy = PyStrategy {
                obj: strategy_obj.clone().unbind(),
            };
            let summary = self.engine.run(&bars, &mut py_strategy);
            Ok(PySummary::from(summary))
        }

        /// Run a Python strategy object over a Parquet file and return a summary.
        fn run_parquet(&mut self, path: &str, strategy_obj: &Bound<'_, PyAny>) -> PyResult<PySummary> {
            let bars = crate::data::load_parquet(path).map_err(|e| {
                PyErr::new::<PyIOError, _>(format!("Parquet load failed: {e}"))
            })?;
            let mut py_strategy = PyStrategy {
                obj: strategy_obj.clone().unbind(),
            };
            let summary = self.engine.run(&bars, &mut py_strategy);
            Ok(PySummary::from(summary))
        }

        /// Run a pre-completed trade schedule through the engine.
        #[pyo3(signature = (trades, topstep=None))]
        fn run_signals(
            &mut self,
            trades: Vec<(i64, i64, i8, f64, f64, f64)>,
            topstep: Option<Py<PyTopstepConfig>>,
        ) -> PyResult<PySummary> {
            let mut prefilled: Vec<PrefilledTrade> = trades
                .into_iter()
                .map(|(entry_ts, exit_ts, direction, entry_price, exit_price, pnl_points)| {
                    PrefilledTrade {
                        entry_ts,
                        exit_ts,
                        direction,
                        entry_price,
                        exit_price,
                        pnl_points,
                    }
                })
                .collect();
            let ts = topstep.as_ref().map(|cfg| {
                Python::with_gil(|py| {
                    let c = cfg.bind(py).get();
                    TopstepConfig {
                        enabled: c.enabled,
                        daily_drawdown_limit: c.daily_drawdown_limit,
                        trailing_drawdown_limit: c.trailing_drawdown_limit,
                        profit_target: c.profit_target,
                        reset_on_failure: c.reset_on_failure,
                    }
                })
            }).unwrap_or_default();
            let summary = self.engine.run_signals(&mut prefilled, &ts);
            Ok(PySummary::from(summary))
        }

        /// Return the engine's equity curve as a list of floats.
        fn equity_curve(&self) -> Vec<f64> {
            self.engine.equity_curve().to_vec()
        }

        /// Return the list of closed trades.
        fn trades(&self) -> Vec<(i64, i64, f64, f64, i32, f64)> {
            // (entry_ts, exit_ts, entry_price, exit_price, qty, pnl)
            self.engine
                .trades()
                .iter()
                .map(|t| (t.entry_ts, t.exit_ts, t.entry_price, t.exit_price, t.qty, t.pnl))
                .collect()
        }
    }
}
