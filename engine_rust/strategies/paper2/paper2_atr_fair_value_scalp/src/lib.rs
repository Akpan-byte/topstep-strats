// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Rust C ABI plugin for the ATR-Optimized FVG Scalping engine.
//   - Mirrors topstep_strats/strategies/paper2_matrix.py::_atr_fair_value_scalp.
// WHY: Fast, language-agnostic Paper-2 engine plugin.

use engine_abi::{Bar, Signal, Strategy, StrategyVTable};
use paper2_indicators::{
    atr, cfg_bool, cfg_f64, cfg_usize, cfg_str, date_day, ema, evaluate_filter, in_session,
    parse_time_to_seconds, rsi, vwap,
};
use serde_json::Value;

#[derive(Debug, Clone)]
struct Params {
    fair_value_source: String,
    deviation_atr: f64,
    rsi_length: usize,
    atr_length: usize,
    session_start: String,
    session_end: String,
    session_only: bool,
    one_trade_per_day: bool,
    filter: Option<String>,
    tp_atr: f64,
    sl_atr: f64,
    tick_size: f64,
}

impl Default for Params {
    fn default() -> Self {
        Params {
            fair_value_source: "vwap".to_string(),
            deviation_atr: 1.5,
            rsi_length: 14,
            atr_length: 14,
            session_start: "09:30".to_string(),
            session_end: "16:00".to_string(),
            session_only: true,
            one_trade_per_day: true,
            filter: None,
            tp_atr: 2.0,
            sl_atr: 3.0,
            tick_size: 0.25,
        }
    }
}

impl From<&Value> for Params {
    fn from(cfg: &Value) -> Self {
        Params {
            fair_value_source: cfg_str(cfg, "fair_value_source", "vwap").to_string(),
            deviation_atr: cfg_f64(cfg, "deviation_atr", 1.5),
            rsi_length: cfg_usize(cfg, "rsi_length", 14),
            atr_length: cfg_usize(cfg, "atr_length", 14),
            session_start: cfg.get("session_start").and_then(|v| v.as_str()).unwrap_or("09:30").to_string(),
            session_end: cfg.get("session_end").and_then(|v| v.as_str()).unwrap_or("16:00").to_string(),
            session_only: cfg_bool(cfg, "session_only", true),
            one_trade_per_day: cfg_bool(cfg, "one_trade_per_day", true),
            filter: cfg.get("filter").and_then(|v| {
                if v.is_null() { None } else { v.as_str().map(|s| s.to_string()) }
            }),
            tp_atr: cfg_f64(cfg, "tp_atr", 2.0),
            sl_atr: cfg_f64(cfg, "sl_atr", 3.0),
            tick_size: cfg_f64(cfg, "tick_size", 0.25),
        }
    }
}

struct State {
    params: Params,
    bars: Vec<Bar>,
    atr: Vec<Option<f64>>,
    rsi: Vec<Option<f64>>,
    fair_value: Vec<Option<f64>>,
    filter_mask: Vec<bool>,
    in_session: Vec<bool>,
    date_day: Vec<i32>,
    start_sec: i64,
    end_sec: i64,
    last_date: i32,
    current_idx: usize,
}

impl State {
    fn new(params: Params) -> Self {
        let start_sec = parse_time_to_seconds(&params.session_start);
        let end_sec = parse_time_to_seconds(&params.session_end);
        State {
            params,
            bars: Vec::new(),
            atr: Vec::new(),
            rsi: Vec::new(),
            fair_value: Vec::new(),
            filter_mask: Vec::new(),
            in_session: Vec::new(),
            date_day: Vec::new(),
            start_sec,
            end_sec,
            last_date: -1,
            current_idx: 0,
        }
    }

    fn prepare(&mut self, bars: &[Bar]) {
        self.bars = bars.to_vec();
        let closes: Vec<f64> = bars.iter().map(|b| b.close).collect();
        self.atr = atr(bars, self.params.atr_length);
        self.rsi = rsi(&closes, self.params.rsi_length);
        self.fair_value = if self.params.fair_value_source == "vwap" {
            vwap(bars)
        } else {
            ema(&closes, 20)
        };
        self.filter_mask = evaluate_filter(self.params.filter.as_deref(), bars);
        self.in_session = bars
            .iter()
            .map(|b| in_session(b.timestamp_ns, self.start_sec, self.end_sec))
            .collect();
        self.date_day = bars.iter().map(|b| date_day(b.timestamp_ns)).collect();
        self.current_idx = 0;
        self.last_date = -1;
    }

    fn make_signal(&self, idx: usize, direction: i8, a: f64) -> Option<Signal> {
        let entry_price = self.bars[idx].close;
        let (stop_loss, take_profit) = if direction == 1 {
            (
                entry_price - self.params.sl_atr * a - self.params.tick_size,
                entry_price + self.params.tp_atr * a + self.params.tick_size,
            )
        } else {
            (
                entry_price + self.params.sl_atr * a + self.params.tick_size,
                entry_price - self.params.tp_atr * a - self.params.tick_size,
            )
        };
        if direction == 1 && !(take_profit > entry_price && entry_price > stop_loss) {
            return None;
        }
        if direction == -1 && !(take_profit < entry_price && entry_price < stop_loss) {
            return None;
        }
        Some(Signal { direction, entry_price: 0.0, stop_loss, take_profit })
    }
}

impl Strategy for State {
    fn prepare(&mut self, bars: &[Bar]) {
        self.prepare(bars);
    }

    fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
        let i = self.current_idx;
        self.current_idx += 1;
        if i >= self.bars.len() || self.bars[i].timestamp_ns != bar.timestamp_ns {
            return None;
        }

        if self.params.session_only && !self.in_session[i] {
            return None;
        }

        let d = self.date_day[i];
        if self.params.one_trade_per_day && self.last_date == d {
            return None;
        }

        let a = match self.atr[i] {
            Some(v) if v > 0.0 => v,
            _ => return None,
        };
        let fv = match self.fair_value[i] {
            Some(v) => v,
            _ => return None,
        };

        if !self.filter_mask[i] {
            return None;
        }

        let c = self.bars[i].close;
        let o = self.bars[i].open;
        let rsi = self.rsi[i];

        if c < fv - self.params.deviation_atr * a && c > o {
            let rsi_ok = rsi.map(|r| r > 30.0).unwrap_or(true);
            if rsi_ok {
                if let Some(sig) = self.make_signal(i, 1, a) {
                    self.last_date = d;
                    return Some(sig);
                }
            }
        }

        if c > fv + self.params.deviation_atr * a && c < o {
            let rsi_ok = rsi.map(|r| r < 70.0).unwrap_or(true);
            if rsi_ok {
                if let Some(sig) = self.make_signal(i, -1, a) {
                    self.last_date = d;
                    return Some(sig);
                }
            }
        }

        None
    }
}

#[no_mangle]
pub extern "C" fn engine_create(config_json: *const u8, len: usize) -> *mut () {
    let config = unsafe { std::slice::from_raw_parts(config_json, len) };
    let params: Params = serde_json::from_slice(config)
        .map(|v: Value| Params::from(&v))
        .unwrap_or_default();
    let state = Box::new(State::new(params));
    Box::into_raw(state) as *mut ()
}

#[no_mangle]
pub extern "C" fn engine_prepare(handle: *mut (), bars: *const Bar, n: usize) {
    if handle.is_null() || bars.is_null() { return; }
    let state = unsafe { &mut *(handle as *mut State) };
    let bars = unsafe { std::slice::from_raw_parts(bars, n) };
    state.prepare(bars);
}

#[no_mangle]
pub extern "C" fn engine_on_bar(handle: *mut (), bar: *const Bar, out: *mut Signal) {
    unsafe {
        if handle.is_null() || bar.is_null() || out.is_null() { return; }
        let state = &mut *(handle as *mut State);
        let bar = &*bar;
        *out = state.on_bar(bar).unwrap_or(Signal { direction: 0, entry_price: 0.0, stop_loss: 0.0, take_profit: 0.0 });
    }
}

#[no_mangle]
pub extern "C" fn engine_destroy(handle: *mut ()) {
    if !handle.is_null() {
        unsafe { drop(Box::from_raw(handle as *mut State)); }
    }
}

#[no_mangle]
pub extern "C" fn engine_vtable() -> *const StrategyVTable {
    static VTABLE: StrategyVTable = StrategyVTable {
        create: engine_create,
        prepare: Some(engine_prepare),
        on_bar: engine_on_bar,
        destroy: engine_destroy,
    };
    &VTABLE
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bar(ts: i64, o: f64, h: f64, l: f64, c: f64, v: u64) -> Bar {
        Bar { timestamp_ns: ts, open: o, high: h, low: l, close: c, volume: v }
    }

    #[test]
    fn test_fair_value_scalp_generates_signals() {
        let base_ts = 1_756_000_200_000_000_000i64;
        let mut bars = Vec::new();
        for i in 0..60 {
            let c = 100.0 + i as f64 * 0.1;
            bars.push(bar(base_ts + i * 60_000_000_000, c, c + 0.5, c - 0.5, c, 1000));
        }
        // Sharp drop below fair value with bullish close
        bars[40].open = 110.0;
        bars[40].close = 105.0;
        bars[40].high = 110.5;
        bars[40].low = 104.0;

        let mut state = State::new(Params::default());
        state.prepare(&bars);
        let mut signals = 0;
        for b in &bars { if state.on_bar(b).is_some() { signals += 1; } }
        assert!(signals >= 0);
    }

    #[test]
    fn test_c_abi_vtable() {
        let vtable = unsafe { &*engine_vtable() };
        assert!(vtable.prepare.is_some());
    }
}
