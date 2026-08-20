// CHANGE_SUMMARY
// 2026-08-20  coder
//   - Rust C ABI plugin for Casper SMC Inverted FVG Paper-1 strategy.
// WHY: Language-agnostic high-performance backtest plugin.

use engine_abi::{Bar, Signal, StrategyVTable};
use paper1_common::{evaluate_filter, in_session, ny_date_key, atr_from_series, FvgState, Resampler, ResampledBar, StrategyConfig};

struct CasperState {
    cfg: StrategyConfig,
    bars_1m: Vec<Bar>,
    resampler: Resampler,
    completed_5m: Vec<ResampledBar>,
    fvg: FvgState,
    last_date: Option<i64>,
}

impl CasperState {
    fn new(cfg_json: &str) -> Option<Box<Self>> {
        let cfg: StrategyConfig = serde_json::from_str(cfg_json).ok()?;
        let tf = cfg.fvg_timeframe.clone();
        Some(Box::new(CasperState {
            cfg,
            bars_1m: Vec::new(),
            resampler: Resampler::new(&tf),
            completed_5m: Vec::new(),
            fvg: FvgState::new(),
            last_date: None,
        }))
    }

    fn evaluate_5m(&mut self, bar_5m: &ResampledBar) -> Option<Signal> {
        let n = self.completed_5m.len();
        if n < 1 {
            return None;
        }

        let close_model = self.cfg.entry_model == "close_back_inside";

        let bull_bottom = self.fvg.last_bull_bottom;
        let bull_top = self.fvg.last_bull_top;
        let bear_top = self.fvg.last_bear_top;
        let bear_bottom = self.fvg.last_bear_bottom;

        let bull_gap_ok = self.fvg.bull_active && !bull_bottom.is_nan();
        let bear_gap_ok = self.fvg.bear_active && !bear_top.is_nan();

        // Bearish inverted FVG: bullish gap gets filled -> short.
        if bull_gap_ok && bar_5m.low <= bull_bottom {
            let confirmed = if close_model {
                bar_5m.close < bull_bottom
            } else {
                bar_5m.high > bull_top
            };
            if confirmed {
                return Some(Signal { direction: -1, entry_price: bar_5m.close, stop_loss: 0.0, take_profit: 0.0 });
            }
        }

        // Bullish inverted FVG: bearish gap gets filled -> long.
        if bear_gap_ok && bar_5m.high >= bear_top {
            let confirmed = if close_model {
                bar_5m.close > bear_top
            } else {
                bar_5m.low < bear_bottom
            };
            if confirmed {
                return Some(Signal { direction: 1, entry_price: bar_5m.close, stop_loss: 0.0, take_profit: 0.0 });
            }
        }

        None
    }

    fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
        if self.cfg.session_only && !in_session(bar.timestamp_ns, &self.cfg.session_start, &self.cfg.session_end) {
            return None;
        }

        self.bars_1m.push(*bar);
        if self.bars_1m.len() > 100 {
            self.bars_1m.remove(0);
        }

        let completed = self.resampler.feed(bar);
        if completed.is_none() {
            return None;
        }
        let bar_5m = completed.unwrap();

        // Evaluate on the completed 5m bar using FVG state from previous 5m bars.
        let signal = self.evaluate_5m(&bar_5m);

        // Update FVG state with the new 5m bar.
        let n = self.completed_5m.len();
        if n >= 2 {
            self.fvg.update(bar_5m.high, bar_5m.low, self.completed_5m[n - 2].high, self.completed_5m[n - 2].low);
        } else if n == 1 {
            self.fvg.update(bar_5m.high, bar_5m.low, self.completed_5m[0].high, self.completed_5m[0].low);
        } else {
            // First bar: no two-bars-ago data, skip FVG creation but still track for next bars.
        }
        self.completed_5m.push(bar_5m);
        if self.completed_5m.len() > 200 {
            self.completed_5m.remove(0);
        }

        // Filter, ATR and one-trade-per-day are checked on the 5m signal bar.
        let sig = signal?;
        let date = ny_date_key(bar.timestamp_ns);
        if self.cfg.one_trade_per_day && self.last_date == Some(date) {
            return None;
        }

        // ATR and filter use the underlying 1m history aligned with the signal bar.
        let close: Vec<f64> = self.bars_1m.iter().map(|b| b.close).collect();
        let high: Vec<f64> = self.bars_1m.iter().map(|b| b.high).collect();
        let low: Vec<f64> = self.bars_1m.iter().map(|b| b.low).collect();
        let atr = atr_from_series(&high, &low, &close, self.cfg.atr_length);
        if atr.map(|a| a <= 0.0).unwrap_or(true) {
            return None;
        }
        if !evaluate_filter(&self.cfg, &self.bars_1m).unwrap_or(false) {
            return None;
        }

        self.last_date = Some(date);
        Some(sig)
    }
}

#[no_mangle]
pub extern "C" fn strategy_create(config_json: *const u8, config_len: usize) -> *mut () {
    if config_json.is_null() { return std::ptr::null_mut(); }
    let slice = unsafe { std::slice::from_raw_parts(config_json, config_len) };
    let cfg_str = std::str::from_utf8(slice).unwrap_or("");
    match CasperState::new(cfg_str) {
        Some(state) => Box::into_raw(state) as *mut (),
        None => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn strategy_on_bar(handle: *mut (), bar: *const Bar, out: *mut Signal) {
    if handle.is_null() || bar.is_null() || out.is_null() { return; }
    let state = unsafe { &mut *(handle as *mut CasperState) };
    unsafe {
        *out = state.on_bar(&*bar).unwrap_or(Signal { direction: 0, entry_price: 0.0, stop_loss: 0.0, take_profit: 0.0 });
    }
}

#[no_mangle]
pub extern "C" fn strategy_destroy(handle: *mut ()) {
    if !handle.is_null() {
        unsafe { let _ = Box::from_raw(handle as *mut CasperState); }
    }
}

static VTABLE: StrategyVTable = StrategyVTable {
    create: strategy_create,
    prepare: None,
    on_bar: strategy_on_bar,
    destroy: strategy_destroy,
};

#[no_mangle]
pub extern "C" fn strategy_vtable() -> *const StrategyVTable {
    &VTABLE
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bar(ts: i64, o: f64, h: f64, l: f64, c: f64, v: u64) -> Bar {
        Bar { timestamp_ns: ts, open: o, high: h, low: l, close: c, volume: v }
    }

    #[test]
    fn test_vtable_exported() {
        assert!(!strategy_vtable().is_null());
    }

    #[test]
    fn test_casper_create_and_run() {
        let cfg = r#"{"blueprint":"casper_inverted_fvg","filter":null,"session_only":false,"one_trade_per_day":false,"entry_model":"close_back_inside"}"#;
        let handle = strategy_create(cfg.as_ptr(), cfg.len());
        assert!(!handle.is_null());

        let base_ts = 1705408200i64 * 1_000_000_000;
        // Feed 30 1m bars in a tight range to build FVG state.
        for i in 0..30 {
            let c = 100.0 + i as f64 * 0.01;
            let b = bar(base_ts + i * 60 * 1_000_000_000, c - 0.01, c + 0.01, c - 0.01, c, 100);
            let mut out = Signal::default();
            strategy_on_bar(handle, &b, &mut out);
        }
        strategy_destroy(handle);
    }
}
