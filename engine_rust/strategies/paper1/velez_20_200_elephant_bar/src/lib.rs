// CHANGE_SUMMARY
// 2026-08-20  coder
//   - Rust C ABI plugin for Velez 20/200 Elephant Bar Paper-1 strategy.
//   - Streaming implementation matching the closed-bar / shift(1) semantics of
//     topstep_strats/strategies/paper1_matrix.py.
// WHY: Language-agnostic high-performance backtest plugin.

use engine_abi::{Bar, Signal, StrategyVTable};
use paper1_common::{evaluate_filter, in_session, ny_date_key, sma, ema, atr_from_series, StrategyConfig};
struct VelezState {
    cfg: StrategyConfig,
    bars: Vec<Bar>,
    max_history: usize,
    last_date: Option<i64>,
}

impl VelezState {
    fn new(cfg_json: &str) -> Option<Box<Self>> {
        let cfg: StrategyConfig = serde_json::from_str(cfg_json).ok()?;
        Some(Box::new(VelezState {
            cfg,
            bars: Vec::new(),
            max_history: 300,
            last_date: None,
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
        if n < 3 {
            return None;
        }

        // Indicators are evaluated at the current bar using closed-bar data only
        // (shift(1)).  Pass the full bar history so indicator values line up with
        // the current bar, matching the pandas semantics in paper1_matrix.py.
        let close: Vec<f64> = self.bars.iter().map(|b| b.close).collect();
        let high: Vec<f64> = self.bars.iter().map(|b| b.high).collect();
        let low: Vec<f64> = self.bars.iter().map(|b| b.low).collect();

        let atr = atr_from_series(&high, &low, &close, self.cfg.atr_length)?;
        if atr <= 0.0 {
            return None;
        }
        let ema_f = ema(&close, self.cfg.ema_fast)?;
        let sma_s = sma(&close, self.cfg.ema_slow)?;

        let filter_ok = evaluate_filter(&self.cfg, &self.bars).unwrap_or(false);
        if !filter_ok {
            return None;
        }

        let cur = &self.bars[n - 1];
        let bar_range = cur.high - cur.low;
        if bar_range < atr * self.cfg.min_range_atr {
            return None;
        }

        let (near_high, near_low) = if self.cfg.close_position == "extreme" {
            (
                (cur.high - cur.close) <= 0.2 * bar_range,
                (cur.close - cur.low) <= 0.2 * bar_range,
            )
        } else {
            (true, true)
        };

        let date = ny_date_key(cur.timestamp_ns);
        if self.cfg.one_trade_per_day && self.last_date == Some(date) {
            return None;
        }

        let long_cond = cur.close > ema_f && cur.close > sma_s && near_high;
        let short_cond = cur.close < ema_f && cur.close < sma_s && near_low;

        if long_cond {
            self.last_date = Some(date);
            return Some(Signal {
                direction: 1,
                entry_price: 0.0,
                stop_loss: 0.0,
                take_profit: 0.0,
            });
        }
        if short_cond {
            self.last_date = Some(date);
            return Some(Signal {
                direction: -1,
                entry_price: 0.0,
                stop_loss: 0.0,
                take_profit: 0.0,
            });
        }
        None
    }
}

#[no_mangle]
pub extern "C" fn strategy_create(config_json: *const u8, config_len: usize) -> *mut () {
    if config_json.is_null() {
        return std::ptr::null_mut();
    }
    let slice = unsafe { std::slice::from_raw_parts(config_json, config_len) };
    let cfg_str = match std::str::from_utf8(slice) {
        Ok(s) => s,
        Err(_) => return std::ptr::null_mut(),
    };
    match VelezState::new(cfg_str) {
        Some(state) => Box::into_raw(state) as *mut (),
        None => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn strategy_on_bar(handle: *mut (), bar: *const Bar, out: *mut Signal) {
    if handle.is_null() || bar.is_null() || out.is_null() {
        return;
    }
    let state = unsafe { &mut *(handle as *mut VelezState) };
    unsafe {
        if let Some(sig) = state.on_bar(&*bar) {
            *out = sig;
        } else {
            *out = Signal {
                direction: 0,
                entry_price: 0.0,
                stop_loss: 0.0,
                take_profit: 0.0,
            };
        }
    }
}

#[no_mangle]
pub extern "C" fn strategy_destroy(handle: *mut ()) {
    if !handle.is_null() {
        unsafe {
            let _ = Box::from_raw(handle as *mut VelezState);
        }
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
        let vtable = strategy_vtable();
        assert!(!vtable.is_null());
    }

    #[test]
    fn test_create_and_run() {
        let cfg = r#"{"blueprint":"velez_20_200_elephant_bar","filter":null,"session_only":false,"one_trade_per_day":false}"#;
        let handle = strategy_create(cfg.as_ptr(), cfg.len());
        assert!(!handle.is_null());

        // Create a large bullish elephant bar after enough warmup.
        // 2024-01-16 14:30 UTC = 09:30 NY
        let base_ts = 1705408200i64 * 1_000_000_000;
        for i in 0..250 {
            let c = 100.0 + i as f64 * 0.1;
            let b = bar(base_ts + i * 60 * 1_000_000_000, c - 0.05, c + 0.05, c - 0.06, c, 100);
            let mut out = Signal::default();
            strategy_on_bar(handle, &b, &mut out);
        }
        let big = bar(base_ts + 250 * 60 * 1_000_000_000, 124.0, 126.0, 123.9, 125.9, 500);
        let mut out = Signal::default();
        strategy_on_bar(handle, &big, &mut out);
        assert_eq!(out.direction, 1);

        strategy_destroy(handle);
    }
}
