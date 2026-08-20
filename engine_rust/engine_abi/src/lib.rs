// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Split lightweight C ABI types out of agnostic_backtest into engine_abi.
// WHY: Strategy cdylib crates only need Bar/Signal/StrategyVTable/ReplayIter.
//      Depending on the full agnostic_backtest rlib pulled in pyo3, parquet and
//      arrow, bloating each plugin link to >2 GB and exhausting disk space.

/// Canonical OHLCV bar.  Kept layout-stable so C/Python/Rust all share it.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Bar {
    pub timestamp_ns: i64,
    pub open: f64,
    pub high: f64,
    pub low: f64,
    pub close: f64,
    pub volume: u64,
}

/// Trading signal returned by a strategy for a single bar.
/// direction: +1 long, -1 short, 0 flat.
/// entry_price: 0.0 means market order (executed at next bar open).
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct Signal {
    pub direction: i8,
    pub entry_price: f64,
    pub stop_loss: f64,
    pub take_profit: f64,
}

/// C ABI function-pointer table for dynamic strategy libraries.
#[repr(C)]
pub struct StrategyVTable {
    pub create: extern "C" fn(config_json: *const u8, config_len: usize) -> *mut (),
    /// Optional full-series preparation. Set to null if unused.
    pub prepare: Option<extern "C" fn(handle: *mut (), bars: *const Bar, n: usize)>,
    pub on_bar: extern "C" fn(handle: *mut (), bar: *const Bar, out: *mut Signal),
    pub destroy: extern "C" fn(handle: *mut ()),
}

/// Core strategy trait implemented by Rust strategies and by language adapters.
/// Engine-agnostic; lives in the lightweight ABI crate so plugins can implement
/// it in unit tests without pulling in the full engine dependencies.
pub trait Strategy {
    /// Optional one-time preparation with the full bar series.
    fn prepare(&mut self, _bars: &[Bar]) {}

    fn on_bar(&mut self, bar: &Bar) -> Option<Signal>;
}

/// Zero-copy replay iterator over a slice of bars.
pub struct ReplayIter<'a> {
    bars: &'a [Bar],
    idx: usize,
}

impl<'a> ReplayIter<'a> {
    pub fn new(bars: &'a [Bar]) -> Self {
        ReplayIter { bars, idx: 0 }
    }
}

impl<'a> Iterator for ReplayIter<'a> {
    type Item = &'a Bar;

    fn next(&mut self) -> Option<Self::Item> {
        let item = self.bars.get(self.idx);
        self.idx += 1;
        item
    }
}
