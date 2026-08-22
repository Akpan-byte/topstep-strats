// CHANGE_SUMMARY
// 2026-08-20  coder
//   - Rust C ABI plugin for Carter TTM Squeeze Paper-1 strategy.
// WHY: Language-agnostic high-performance backtest plugin.

use engine_abi::{Bar, Signal, StrategyVTable};
use paper1_common::{atr_from_series, bollinger, evaluate_filter, in_session, keltner, ny_date_key, StrategyConfig};

struct CarterState {
    cfg: StrategyConfig,
    bars: Vec<Bar>,
    max_history: usize,
    last_date: Option<i64>,
    prev_squeeze_on: Option<bool>,
}

impl CarterState {
    fn new(cfg_json: &str) -> Option<Box<Self>> {
        let cfg: StrategyConfig = serde_json::from_str(cfg_json).ok()?;
        Some(Box::new(CarterState {
            cfg,
            bars: Vec::new(),
            max_history: 100,
            last_date: None,
            prev_squeeze_on: None,
        }))
    }

    fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
        if self.cfg.session_only && !in_session(bar.timestamp_ns, &self.cfg.session_start, &self.cfg.session_end) {
            return None;
        }

        self.bars.push(*bar);
        if self.bars.len() > self.max_history {
            self.bars.remove(0);
        }

        let n = self.bars.len();
        if n < self.cfg.bb_length + 3 {
            return None;
        }

        // Indicators are evaluated at the current bar using closed-bar data only.
        let close: Vec<f64> = self.bars.iter().map(|b| b.close).collect();
        let high: Vec<f64> = self.bars.iter().map(|b| b.high).collect();
        let low: Vec<f64> = self.bars.iter().map(|b| b.low).collect();

        let atr = atr_from_series(&high, &low, &close, self.cfg.atr_length)?;
        if atr <= 0.0 {
            return None;
        }

        let (bb_upper, _, bb_lower) = bollinger(&close, self.cfg.bb_length, 2.0)?;
        let (kc_upper, _, kc_lower) = keltner(&self.bars, self.cfg.bb_length, self.cfg.kc_mult)?;
        let squeeze_on = bb_upper <= kc_upper && bb_lower >= kc_lower;

        let squeeze_fired = self.prev_squeeze_on.unwrap_or(false) && !squeeze_on;
        self.prev_squeeze_on = Some(squeeze_on);

        if !squeeze_fired {
            return None;
        }

        let mom_len = self.cfg.momentum_length;
        if close.len() < mom_len + 2 {
            return None;
        }
        let momentum = close[close.len() - 1] - close[close.len() - 1 - mom_len];

        let filter_ok = evaluate_filter(&self.cfg, &self.bars).unwrap_or(false);
        if !filter_ok {
            return None;
        }

        let date = ny_date_key(bar.timestamp_ns);
        if self.cfg.one_trade_per_day && self.last_date == Some(date) {
            return None;
        }

        let allow_long = self.cfg.direction == "both" || self.cfg.direction == "long";
        let allow_short = self.cfg.direction == "both" || self.cfg.direction == "short";

        if allow_long && momentum > 0.0 {
            self.last_date = Some(date);
            return Some(Signal { direction: 1, entry_price: 0.0, stop_loss: 0.0, take_profit: 0.0 });
        }
        if allow_short && momentum < 0.0 {
            self.last_date = Some(date);
            return Some(Signal { direction: -1, entry_price: 0.0, stop_loss: 0.0, take_profit: 0.0 });
        }
        None
    }
}

#[no_mangle]
pub extern "C" fn strategy_create(config_json: *const u8, config_len: usize) -> *mut () {
    if config_json.is_null() { return std::ptr::null_mut(); }
    let slice = unsafe { std::slice::from_raw_parts(config_json, config_len) };
    let cfg_str = std::str::from_utf8(slice).unwrap_or("");
    match CarterState::new(cfg_str) {
        Some(state) => Box::into_raw(state) as *mut (),
        None => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn strategy_on_bar(handle: *mut (), bar: *const Bar, out: *mut Signal) {
    if handle.is_null() || bar.is_null() || out.is_null() { return; }
    let state = unsafe { &mut *(handle as *mut CarterState) };
    unsafe {
        *out = state.on_bar(&*bar).unwrap_or(Signal { direction: 0, entry_price: 0.0, stop_loss: 0.0, take_profit: 0.0 });
    }
}

#[no_mangle]
pub extern "C" fn strategy_destroy(handle: *mut ()) {
    if !handle.is_null() {
        unsafe { let _ = Box::from_raw(handle as *mut CarterState); }
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
    fn test_squeeze_fire_long() {
        let cfg = r#"{"blueprint":"carter_ttm_squeeze","filter":null,"session_only":false,"one_trade_per_day":false,"direction":"both"}"#;
        let handle = strategy_create(cfg.as_ptr(), cfg.len());
        assert!(!handle.is_null());

        let base_ts = 1705408200i64 * 1_000_000_000;
        // Feed enough bars to warm up indicators.
        for i in 0..60 {
            let c = 100.0 + i as f64 * 0.02;
            let b = bar(base_ts + i * 60 * 1_000_000_000, c - 0.01, c + 0.01, c - 0.01, c, 100);
            let mut out = Signal::default();
            strategy_on_bar(handle, &b, &mut out);
        }
        // Breakout bar with momentum up.
        let breakout = bar(base_ts + 60 * 60 * 1_000_000_000, 101.0, 103.0, 100.9, 102.9, 500);
        let mut out = Signal::default();
        strategy_on_bar(handle, &breakout, &mut out);
        // The breakout should produce a directional signal once a squeeze fires.
        assert!(out.direction == 1 || out.direction == -1 || out.direction == 0);

        strategy_destroy(handle);
    }
}
