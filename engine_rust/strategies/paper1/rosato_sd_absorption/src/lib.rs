// CHANGE_SUMMARY
// 2026-08-20  coder
//   - Rust C ABI plugin for Rosato Supply/Demand Absorption Paper-1 strategy.
//   - Streaming implementation resamples 1m bars to the configured zone
//     timeframe and evaluates demand/supply, absorption, and filter conditions
//     using closed-bar / shift(1) semantics.
// WHY: Language-agnostic high-performance backtest plugin.

use engine_abi::{Bar, Signal, StrategyVTable};
use paper1_common::{
    atr_from_series, ema, evaluate_filter, in_session, ny_date_key, ResampledBar, Resampler,
    StrategyConfig,
};

struct RosatoState {
    cfg: StrategyConfig,
    resampler: Resampler,
    bars_1m: Vec<Bar>,
    zones: Vec<ResampledBar>,
    last_date: Option<i64>,
}

impl RosatoState {
    fn new(cfg_json: &str) -> Option<Box<Self>> {
        let cfg: StrategyConfig = serde_json::from_str(cfg_json).ok()?;
        let tf = cfg.zone_timeframe.clone();
        Some(Box::new(RosatoState {
            cfg,
            resampler: Resampler::new(&tf),
            bars_1m: Vec::new(),
            zones: Vec::new(),
            last_date: None,
        }))
    }

    fn evaluate_zone(&self, idx: usize) -> Option<(i8, f64)> {
        let n = self.zones.len();
        if idx < 1 || idx >= n {
            return None;
        }
        let cur = &self.zones[idx];
        let prev = &self.zones[idx - 1];

        // Volume threshold: rolling 50 quantile(vol_pct/100) shifted by 1.
        let window = 50usize;
        if idx < window + 1 {
            return None;
        }
        let vol_window: Vec<u64> = self.zones[idx - window..idx].iter().map(|z| z.volume).collect();
        let mut vol_sorted: Vec<f64> = vol_window.iter().map(|&v| v as f64).collect();
        vol_sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let q = self.cfg.volume_percentile / 100.0;
        let rank = (q * (vol_sorted.len() - 1) as f64).round() as usize;
        let vol_threshold = vol_sorted[rank.min(vol_sorted.len() - 1)];

        // Demand / supply on the zone bar.
        let demand = cur.low <= prev.low && cur.close > cur.open && (cur.volume as f64) > vol_threshold;
        let supply = cur.high >= prev.high && cur.close < cur.open && (cur.volume as f64) > vol_threshold;

        // Recent demand/supply: shift(1) or shift(2).
        let demand_recent = if idx >= 2 {
            let d1 = self.zones[idx - 1].low <= self.zones[idx - 2].low
                && self.zones[idx - 1].close > self.zones[idx - 1].open;
            let d2 = if idx >= 3 {
                self.zones[idx - 2].low <= self.zones[idx - 3].low
                    && self.zones[idx - 2].close > self.zones[idx - 2].open
            } else {
                false
            };
            d1 || d2
        } else {
            false
        };
        let supply_recent = if idx >= 2 {
            let s1 = self.zones[idx - 1].high >= self.zones[idx - 2].high
                && self.zones[idx - 1].close < self.zones[idx - 1].open;
            let s2 = if idx >= 3 {
                self.zones[idx - 2].high >= self.zones[idx - 3].high
                    && self.zones[idx - 2].close < self.zones[idx - 2].open
            } else {
                false
            };
            s1 || s2
        } else {
            false
        };

        // Absorption rolling window metrics.
        let abs_bars = self.cfg.absorption_bars;
        if idx < abs_bars + 1 {
            return None;
        }
        let window_low: Vec<f64> = self.zones[idx - abs_bars..idx].iter().map(|z| z.low).collect();
        let window_high: Vec<f64> = self.zones[idx - abs_bars..idx].iter().map(|z| z.high).collect();
        let window_vol: Vec<f64> = self.zones[idx - abs_bars..idx].iter().map(|z| z.volume as f64).collect();
        let rolling_low_min = window_low.iter().cloned().fold(f64::INFINITY, f64::min);
        let rolling_high_max = window_high.iter().cloned().fold(f64::NEG_INFINITY, f64::max);
        let rolling_mean_vol = window_vol.iter().sum::<f64>() / window_vol.len() as f64;
        let prev_volume = self.zones[idx - 1].volume as f64;

        // EMA20 and ATR on zone closes up to idx (shift(1) value at idx).
        let close: Vec<f64> = self.zones[..=idx].iter().map(|z| z.close).collect();
        let high: Vec<f64> = self.zones[..=idx].iter().map(|z| z.high).collect();
        let low: Vec<f64> = self.zones[..=idx].iter().map(|z| z.low).collect();
        let ema_val = ema(&close, 20)?;
        let atr = atr_from_series(&high, &low, &close, self.cfg.atr_length)?;
        if atr <= 0.0 {
            return None;
        }

        let filter_ok = evaluate_filter(&self.cfg, &self.bars_1m).unwrap_or(false);
        if !filter_ok {
            return None;
        }

        let long_cond = demand_recent
            && rolling_low_min <= prev.low
            && prev_volume < rolling_mean_vol
            && cur.close > cur.open
            && cur.close > ema_val;
        let short_cond = supply_recent
            && rolling_high_max >= prev.high
            && prev_volume < rolling_mean_vol
            && cur.close < cur.open
            && cur.close < ema_val;

        if long_cond {
            Some((1, cur.close))
        } else if short_cond {
            Some((-1, cur.close))
        } else {
            None
        }
    }

    fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
        if self.cfg.session_only && !in_session(bar.timestamp_ns, &self.cfg.session_start, &self.cfg.session_end) {
            return None;
        }

        self.bars_1m.push(*bar);
        if self.bars_1m.len() > 300 {
            self.bars_1m.remove(0);
        }

        let completed = self.resampler.feed(bar)?;
        self.zones.push(completed);
        if self.zones.len() > 300 {
            self.zones.remove(0);
        }

        let idx = self.zones.len() - 1;
        let (direction, entry_price) = self.evaluate_zone(idx)?;

        let date = ny_date_key(bar.timestamp_ns);
        if self.cfg.one_trade_per_day && self.last_date == Some(date) {
            return None;
        }
        self.last_date = Some(date);

        Some(Signal {
            direction,
            entry_price,
            stop_loss: 0.0,
            take_profit: 0.0,
        })
    }
}

#[no_mangle]
pub extern "C" fn strategy_create(config_json: *const u8, config_len: usize) -> *mut () {
    if config_json.is_null() {
        return std::ptr::null_mut();
    }
    let slice = unsafe { std::slice::from_raw_parts(config_json, config_len) };
    let cfg_str = std::str::from_utf8(slice).unwrap_or("");
    match RosatoState::new(cfg_str) {
        Some(state) => Box::into_raw(state) as *mut (),
        None => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub extern "C" fn strategy_on_bar(handle: *mut (), bar: *const Bar, out: *mut Signal) {
    if handle.is_null() || bar.is_null() || out.is_null() {
        return;
    }
    let state = unsafe { &mut *(handle as *mut RosatoState) };
    unsafe {
        *out = state.on_bar(&*bar).unwrap_or(Signal {
            direction: 0,
            entry_price: 0.0,
            stop_loss: 0.0,
            take_profit: 0.0,
        });
    }
}

#[no_mangle]
pub extern "C" fn strategy_destroy(handle: *mut ()) {
    if !handle.is_null() {
        unsafe {
            let _ = Box::from_raw(handle as *mut RosatoState);
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
        Bar {
            timestamp_ns: ts,
            open: o,
            high: h,
            low: l,
            close: c,
            volume: v,
        }
    }

    #[test]
    fn test_vtable_exported() {
        assert!(!strategy_vtable().is_null());
    }

    #[test]
    fn test_create_and_run() {
        let cfg = r#"{"blueprint":"rosato_sd_absorption","filter":null,"session_only":false,"one_trade_per_day":false}"#;
        let handle = strategy_create(cfg.as_ptr(), cfg.len());
        assert!(!handle.is_null());

        let base_ts = 1705408200i64 * 1_000_000_000;
        // Feed enough 1m bars to complete several 15m zone bars.
        for i in 0..120 {
            let c = 100.0 + i as f64 * 0.02;
            let b = bar(base_ts + i * 60 * 1_000_000_000, c - 0.01, c + 0.01, c - 0.01, c, 100);
            let mut out = Signal::default();
            strategy_on_bar(handle, &b, &mut out);
        }
        strategy_destroy(handle);
    }
}
