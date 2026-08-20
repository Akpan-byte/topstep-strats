// CHANGE_SUMMARY
// 2026-08-20  coder
//   - Created paper1_common: shared indicators, filters, session handling and
//     resampling for the 7 Paper-1 Rust strategy plugins.
// WHY: Keep the 7 cdylib crates DRY and guarantee that indicator math matches
//      the closed-bar, shift(1) semantics used in paper1_matrix.py.

use engine_abi::{Bar, Signal};
use serde::Deserialize;

pub use engine_abi::{Bar as AbiBar, Signal as AbiSignal, StrategyVTable};

// ---------------------------------------------------------------------------
// Time / session helpers
// ---------------------------------------------------------------------------

/// Convert a UTC nanosecond timestamp to America/New_York seconds-of-day.
/// Handles DST for dates from 2007 onward (second Sunday March -> first Sunday
/// November).  This is enough for the data-v2.0 release history.
pub fn ny_seconds_of_day(timestamp_ns: i64) -> i64 {
    let ns_per_s = 1_000_000_000i64;
    let secs = timestamp_ns / ns_per_s;
    let days = secs / 86_400;
    // March 1 2007 is day 732695 in the Unix-epoch day numbering used here.
    let (year, month, day) = unix_epoch_days_to_date(days);
    let in_dst = is_us_dst(year, month, day);
    let offset = if in_dst { -4 * 3600 } else { -5 * 3600 };
    let local_secs = secs + offset;
    ((local_secs % 86_400) + 86_400) % 86_400
}

fn unix_epoch_days_to_date(mut days: i64) -> (i32, u32, u32) {
    // Approximate inverse of days since 1970-01-01.
    let mut year = 1970i32;
    loop {
        let ly = is_leap_year(year);
        let d = if ly { 366 } else { 365 };
        if days < d {
            break;
        }
        days -= d;
        year += 1;
    }
    let month_lengths = if is_leap_year(year) {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };
    let mut month = 1u32;
    for ml in month_lengths {
        if days < ml {
            break;
        }
        days -= ml;
        month += 1;
    }
    (year, month, (days + 1) as u32)
}

fn is_leap_year(year: i32) -> bool {
    (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)
}

fn is_us_dst(year: i32, month: u32, day: u32) -> bool {
    if !(2007..=2100).contains(&year) || month < 3 || month > 11 || (month == 3 && day < 8) || (month == 11 && day > 7) {
        return false;
    }
    if month > 3 && month < 11 {
        return true;
    }
    // DST starts second Sunday of March (earliest is March 8).
    // DST ends first Sunday of November (latest is November 7).
    if month == 3 {
        let first_sun = ((1 + (7 - day_of_week(year, 3, 1)) % 7) % 7 + 1) as u32;
        let second_sun = first_sun + 7;
        day >= second_sun
    } else {
        // November
        let first_sun = ((1 + (7 - day_of_week(year, 11, 1)) % 7) % 7 + 1) as u32;
        day < first_sun
    }
}

fn day_of_week(year: i32, month: u32, day: u32) -> i32 {
    // Tomohiko Sakamoto algorithm
    let t = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4];
    let y = if month < 3 { year - 1 } else { year };
    (y + y / 4 - y / 100 + y / 400 + t[(month - 1) as usize] as i32 + day as i32) % 7
}

pub fn parse_hhmm(s: &str) -> i64 {
    let parts: Vec<_> = s.split(':').collect();
    let h: i64 = parts[0].parse().unwrap_or(0);
    let m: i64 = parts.get(1).and_then(|x| x.parse().ok()).unwrap_or(0);
    h * 3600 + m * 60
}

pub fn in_session(timestamp_ns: i64, start_hhmm: &str, end_hhmm: &str) -> bool {
    let s = parse_hhmm(start_hhmm);
    let e = parse_hhmm(end_hhmm);
    let t = ny_seconds_of_day(timestamp_ns);
    if s <= e {
        t >= s && t <= e
    } else {
        t >= s || t <= e
    }
}

pub fn ny_date_key(timestamp_ns: i64) -> i64 {
    // YYYYMMDD in America/New_York
    let secs = timestamp_ns / 1_000_000_000;
    let offset = if is_us_dst_from_epoch_days(secs / 86_400) {
        -4 * 3600
    } else {
        -5 * 3600
    };
    let local_secs = secs + offset;
    let days = local_secs / 86_400;
    let (y, m, d) = unix_epoch_days_to_date(days);
    y as i64 * 10_000 + m as i64 * 100 + d as i64
}

fn is_us_dst_from_epoch_days(days: i64) -> bool {
    let (y, m, d) = unix_epoch_days_to_date(days);
    is_us_dst(y, m, d)
}

// ---------------------------------------------------------------------------
// Strategy configuration
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Deserialize)]
pub struct StrategyConfig {
    pub blueprint: String,
    #[serde(default = "default_instrument")]
    pub instrument: String,
    #[serde(default = "default_tick_size")]
    pub tick_size: f64,
    #[serde(default = "default_point_value")]
    pub point_value: f64,
    #[serde(default = "default_session")]
    pub session: String,
    #[serde(default = "default_session_start")]
    pub session_start: String,
    #[serde(default = "default_session_end")]
    pub session_end: String,
    #[serde(default = "default_tz")]
    pub tz: String,
    #[serde(default = "default_session_only")]
    pub session_only: bool,
    #[serde(default = "default_one_trade_per_day")]
    pub one_trade_per_day: bool,
    #[serde(default)]
    pub filter: Option<String>,
    #[serde(default = "default_tp_atr")]
    pub tp_atr: f64,
    #[serde(default = "default_sl_atr")]
    pub sl_atr: f64,
    #[serde(default = "default_atr_length")]
    pub atr_length: usize,
    #[serde(default = "default_stop_first")]
    pub stop_first: bool,

    // ICT Silver Bullet
    #[serde(default = "default_sweep_tf")]
    pub sweep_timeframe: String,
    #[serde(default = "default_entry_tf")]
    pub entry_timeframe: String,
    #[serde(default = "default_fvg_required")]
    pub fvg_required: bool,
    #[serde(default = "default_retracement_pct")]
    pub retracement_pct: f64,

    // Casper Inverted FVG
    #[serde(default = "default_fvg_tf")]
    pub fvg_timeframe: String,
    #[serde(default = "default_entry_model")]
    pub entry_model: String,

    // Velez
    #[serde(default = "default_ema_fast")]
    pub ema_fast: usize,
    #[serde(default = "default_ema_slow")]
    pub ema_slow: usize,
    #[serde(default = "default_min_range_atr")]
    pub min_range_atr: f64,
    #[serde(default = "default_close_position")]
    pub close_position: String,

    // Rosato
    #[serde(default = "default_zone_tf")]
    pub zone_timeframe: String,
    #[serde(default = "default_volume_percentile")]
    pub volume_percentile: f64,
    #[serde(default = "default_absorption_bars")]
    pub absorption_bars: usize,

    // Carter
    #[serde(default = "default_bb_length")]
    pub bb_length: usize,
    #[serde(default = "default_kc_mult")]
    pub kc_mult: f64,
    #[serde(default = "default_momentum_length")]
    pub momentum_length: usize,
    #[serde(default = "default_direction")]
    pub direction: String,

    // Raschke
    #[serde(default = "default_ema_length")]
    pub ema_length: usize,
    #[serde(default = "default_adx_length")]
    pub adx_length: usize,
    #[serde(default = "default_adx_threshold")]
    pub adx_threshold: f64,
    #[serde(default = "default_pullback_depth")]
    pub pullback_depth: f64,

    // Wade PATs
    #[serde(default = "default_swing_lookback")]
    pub swing_lookback: usize,
    #[serde(default)]
    pub trend_filter: bool,
}

fn default_instrument() -> String { "NQ".into() }
fn default_tick_size() -> f64 { 0.25 }
fn default_point_value() -> f64 { 20.0 }
fn default_session() -> String { "NY".into() }
fn default_session_start() -> String { "09:30".into() }
fn default_session_end() -> String { "16:00".into() }
fn default_tz() -> String { "America/New_York".into() }
fn default_session_only() -> bool { true }
fn default_one_trade_per_day() -> bool { true }
fn default_tp_atr() -> f64 { 2.0 }
fn default_sl_atr() -> f64 { 5.0 }
fn default_atr_length() -> usize { 14 }
fn default_stop_first() -> bool { true }
fn default_sweep_tf() -> String { "15m".into() }
fn default_entry_tf() -> String { "1m".into() }
fn default_fvg_required() -> bool { true }
fn default_retracement_pct() -> f64 { 0.5 }
fn default_fvg_tf() -> String { "5m".into() }
fn default_entry_model() -> String { "close_back_inside".into() }
fn default_ema_fast() -> usize { 20 }
fn default_ema_slow() -> usize { 200 }
fn default_min_range_atr() -> f64 { 1.0 }
fn default_close_position() -> String { "extreme".into() }
fn default_zone_tf() -> String { "15m".into() }
fn default_volume_percentile() -> f64 { 80.0 }
fn default_absorption_bars() -> usize { 3 }
fn default_bb_length() -> usize { 20 }
fn default_kc_mult() -> f64 { 1.5 }
fn default_momentum_length() -> usize { 12 }
fn default_direction() -> String { "both".into() }
fn default_ema_length() -> usize { 20 }
fn default_adx_length() -> usize { 14 }
fn default_adx_threshold() -> f64 { 25.0 }
fn default_pullback_depth() -> f64 { 0.5 }
fn default_swing_lookback() -> usize { 10 }

// ---------------------------------------------------------------------------
// Bar series helpers
// ---------------------------------------------------------------------------

pub struct BarSeries {
    pub bars: Vec<Bar>,
    pub max_history: usize,
}

impl BarSeries {
    pub fn new(max_history: usize) -> Self {
        BarSeries { bars: Vec::new(), max_history }
    }

    pub fn push(&mut self, bar: Bar) {
        self.bars.push(bar);
        if self.bars.len() > self.max_history {
            self.bars.remove(0);
        }
    }

    pub fn len(&self) -> usize { self.bars.len() }
    pub fn is_empty(&self) -> bool { self.bars.is_empty() }

    pub fn close(&self, idx: usize) -> f64 { self.bars[idx].close }
    pub fn high(&self, idx: usize) -> f64 { self.bars[idx].high }
    pub fn low(&self, idx: usize) -> f64 { self.bars[idx].low }
    pub fn open(&self, idx: usize) -> f64 { self.bars[idx].open }
    pub fn volume(&self, idx: usize) -> u64 { self.bars[idx].volume }
    pub fn ts(&self, idx: usize) -> i64 { self.bars[idx].timestamp_ns }
}

// ---------------------------------------------------------------------------
// Incremental indicators (closed-bar / shift(1) semantics)
// ---------------------------------------------------------------------------

pub fn sma(values: &[f64], length: usize) -> Option<f64> {
    if values.len() < length + 1 {
        return None;
    }
    let slice = &values[values.len() - length - 1..values.len() - 1];
    Some(slice.iter().sum::<f64>() / length as f64)
}

pub fn ema(values: &[f64], length: usize) -> Option<f64> {
    if values.len() < length + 1 {
        return None;
    }
    let alpha = 2.0 / (length as f64 + 1.0);
    // pandas ewm(span=length, adjust=False, min_periods=length).mean().shift(1):
    // first valid value is SMA of the first `length` points, then recursive.
    // The returned value corresponds to the bar before the last in `values`.
    let first_sma: f64 = values[..length].iter().sum::<f64>() / length as f64;
    let mut ema = first_sma;
    for i in length..(values.len() - 1) {
        ema = alpha * values[i] + (1.0 - alpha) * ema;
    }
    Some(ema)
}

pub fn true_range(bar: &Bar, prev_close: f64) -> f64 {
    let r1 = bar.high - bar.low;
    let r2 = (bar.high - prev_close).abs();
    let r3 = (bar.low - prev_close).abs();
    r1.max(r2).max(r3)
}

pub fn atr_from_series(high: &[f64], low: &[f64], close: &[f64], length: usize) -> Option<f64> {
    if close.len() < length + 2 {
        return None;
    }
    // true range uses previous close, so we need length+1 bars of true range
    // then rolling mean of length values, shifted by 1 => value at index i uses
    // TR at indices [i-length, i-1].
    let n = close.len();
    let mut tr = Vec::with_capacity(n - 1);
    for i in 1..n {
        let prev_close = close[i - 1];
        let r1 = high[i] - low[i];
        let r2 = (high[i] - prev_close).abs();
        let r3 = (low[i] - prev_close).abs();
        tr.push(r1.max(r2).max(r3));
    }
    if tr.len() < length + 1 {
        return None;
    }
    let slice = &tr[tr.len() - length - 1..tr.len() - 1];
    Some(slice.iter().sum::<f64>() / length as f64)
}

pub fn rsi(close: &[f64], length: usize) -> Option<f64> {
    if close.len() < length + 2 {
        return None;
    }
    let n = close.len();
    let mut gains = Vec::with_capacity(n - 1);
    let mut losses = Vec::with_capacity(n - 1);
    for i in 1..(n - 1) {
        let d = close[i] - close[i - 1];
        gains.push(d.max(0.0));
        losses.push((-d).max(0.0));
    }
    if gains.len() < length {
        return None;
    }
    let avg_gain: f64 = gains[gains.len() - length..].iter().sum::<f64>() / length as f64;
    let avg_loss: f64 = losses[losses.len() - length..].iter().sum::<f64>() / length as f64;
    if avg_loss == 0.0 {
        return None;
    }
    let rs = avg_gain / avg_loss;
    Some(100.0 - (100.0 / (1.0 + rs)))
}

pub fn macd_hist(close: &[f64], fast: usize, slow: usize, signal: usize) -> Option<f64> {
    let ema_fast = ema_with_len(close, fast)?;
    let ema_slow = ema_with_len(close, slow)?;
    let macd = ema_fast - ema_slow;
    // signal line: EMA of macd series.  For parity we need the EMA of the MACD
    // values up to the previous bar.  Approximate with incremental EMA of MACD.
    // Recompute macd values for all available bars.
    let mut macd_vals = Vec::new();
    for i in (slow + 1)..(close.len()) {
        let ef = ema_at(close, fast, i)?;
        let es = ema_at(close, slow, i)?;
        macd_vals.push(ef - es);
    }
    if macd_vals.len() < signal + 1 {
        return None;
    }
    let signal_ema = ema_direct(&macd_vals[..macd_vals.len() - 1], signal)?;
    Some(macd - signal_ema)
}

fn ema_with_len(values: &[f64], length: usize) -> Option<f64> {
    ema(values, length)
}

fn ema_direct(values: &[f64], length: usize) -> Option<f64> {
    if values.len() < length {
        return None;
    }
    let alpha = 2.0 / (length as f64 + 1.0);
    let first_sma: f64 = values[..length].iter().sum::<f64>() / length as f64;
    let mut ema = first_sma;
    for i in length..values.len() {
        ema = alpha * values[i] + (1.0 - alpha) * ema;
    }
    Some(ema)
}

fn ema_at(values: &[f64], length: usize, end_exclusive: usize) -> Option<f64> {
    if end_exclusive < length + 1 {
        return None;
    }
    ema(&values[..end_exclusive], length)
}

pub fn vwap_up_to(bars: &[Bar], idx: usize) -> Option<f64> {
    // Daily anchored VWAP using bars up to idx-1, matching shift(1).
    if idx == 0 {
        return None;
    }
    let current_date = ny_date_key(bars[idx].timestamp_ns);
    let mut cum_tp_vol = 0.0;
    let mut cum_vol = 0.0;
    for j in (0..idx).rev() {
        if ny_date_key(bars[j].timestamp_ns) != current_date {
            break;
        }
        let tp = (bars[j].high + bars[j].low + bars[j].close) / 3.0;
        let v = bars[j].volume as f64;
        cum_tp_vol += tp * v;
        cum_vol += v;
    }
    if cum_vol == 0.0 {
        return None;
    }
    Some(cum_tp_vol / cum_vol)
}

pub fn volume_sma(volume: &[u64], length: usize) -> Option<f64> {
    if volume.len() < length + 1 {
        return None;
    }
    let slice = &volume[volume.len() - length - 1..volume.len() - 1];
    Some(slice.iter().map(|&v| v as f64).sum::<f64>() / length as f64)
}

pub fn bollinger(close: &[f64], length: usize, std_dev: f64) -> Option<(f64, f64, f64)> {
    if close.len() < length + 1 {
        return None;
    }
    let slice = &close[close.len() - length - 1..close.len() - 1];
    let mean = slice.iter().sum::<f64>() / length as f64;
    let variance = slice.iter().map(|&x| (x - mean).powi(2)).sum::<f64>() / (length as f64 - 1.0);
    let std = variance.sqrt();
    Some((mean + std_dev * std, mean, mean - std_dev * std))
}

pub fn keltner(bars: &[Bar], length: usize, mult: f64) -> Option<(f64, f64, f64)> {
    let close: Vec<f64> = bars.iter().map(|b| b.close).collect();
    let mid = ema(&close, length)?;
    let atr = atr_from_series(
        &bars.iter().map(|b| b.high).collect::<Vec<_>>(),
        &bars.iter().map(|b| b.low).collect::<Vec<_>>(),
        &close,
        length,
    )?;
    Some((mid + mult * atr, mid, mid - mult * atr))
}

pub fn adx(bars: &[Bar], length: usize) -> Option<f64> {
    if bars.len() < 2 * length + 2 {
        return None;
    }
    let n = bars.len();
    let mut plus_dm = Vec::with_capacity(n - 1);
    let mut minus_dm = Vec::with_capacity(n - 1);
    let mut tr = Vec::with_capacity(n - 1);
    for i in 1..n {
        let up = bars[i].high - bars[i - 1].high;
        let down = bars[i - 1].low - bars[i].low;
        let pdm = if up > down && up > 0.0 { up } else { 0.0 };
        let mdm = if down > up && down > 0.0 { down } else { 0.0 };
        plus_dm.push(pdm);
        minus_dm.push(mdm);
        let prev_close = bars[i - 1].close;
        tr.push((bars[i].high - bars[i].low)
            .max((bars[i].high - prev_close).abs())
            .max((bars[i].low - prev_close).abs()));
    }
    // We need ADX at index n-1 computed from values shifted by 1.
    // Use DM/TR up to index n-2 for the smoothed sums, then compute DX up to n-2,
    // then rolling mean of DX over length values ending at n-2.
    let end = n - 2;
    if end + 1 < length {
        return None;
    }
    let atr_slice = &tr[end + 1 - length..=end];
    let atr_sum: f64 = atr_slice.iter().sum();
    let pdm_slice = &plus_dm[end + 1 - length..=end];
    let mdm_slice = &minus_dm[end + 1 - length..=end];
    let pdi = 100.0 * pdm_slice.iter().sum::<f64>() / atr_sum;
    let mdi = 100.0 * mdm_slice.iter().sum::<f64>() / atr_sum;
    let denom = pdi + mdi;
    if denom == 0.0 {
        return Some(0.0);
    }
    let dx = (pdi - mdi).abs() / denom * 100.0;
    // For ADX we need a rolling mean of DX over length values ending at end.
    // Recompute a proper DX series for available history.
    let mut dx_series = Vec::new();
    for i in (length - 1)..=end {
        let atr_sum: f64 = tr[i + 1 - length..=i].iter().sum();
        let pdi = 100.0 * plus_dm[i + 1 - length..=i].iter().sum::<f64>() / atr_sum;
        let mdi = 100.0 * minus_dm[i + 1 - length..=i].iter().sum::<f64>() / atr_sum;
        let denom = pdi + mdi;
        let dx = if denom == 0.0 { 0.0 } else { (pdi - mdi).abs() / denom * 100.0 };
        dx_series.push(dx);
    }
    if dx_series.len() < length {
        return Some(dx);
    }
    let dx_mean = dx_series[dx_series.len() - length..].iter().sum::<f64>() / length as f64;
    Some(dx_mean)
}

// ---------------------------------------------------------------------------
// FVG and swing helpers
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default)]
pub struct FvgState {
    pub last_bull_top: f64,
    pub last_bull_bottom: f64,
    pub last_bear_top: f64,
    pub last_bear_bottom: f64,
    pub bull_active: bool,
    pub bear_active: bool,
}

impl FvgState {
    pub fn new() -> Self {
        FvgState::default()
    }

    pub fn update(&mut self, high: f64, low: f64, high_2: f64, low_2: f64) {
        // high_2/low_2 are the high/low two bars ago.
        if self.bull_active && low <= self.last_bull_bottom {
            self.bull_active = false;
        }
        if self.bear_active && high >= self.last_bear_top {
            self.bear_active = false;
        }
        if low > high_2 {
            self.last_bull_bottom = high_2;
            self.last_bull_top = low;
            self.bull_active = true;
        }
        if high < low_2 {
            self.last_bear_top = low_2;
            self.last_bear_bottom = high;
            self.bear_active = true;
        }
    }
}

pub fn swing_high_low(bars: &[Bar], lookback: usize, idx: usize) -> (bool, bool) {
    // pandas: center=True, min_periods=lookback, shift(1)
    // At bar idx (current bar being evaluated), swing markers come from bar idx-1.
    if idx < lookback * 2 + 2 || idx > bars.len() {
        return (false, false);
    }
    let center = idx - 1;
    let start = center.saturating_sub(lookback);
    let end = (center + lookback + 1).min(bars.len());
    let mut max_high = bars[start].high;
    let mut min_low = bars[start].low;
    for i in start..end {
        if i == center {
            continue;
        }
        max_high = max_high.max(bars[i].high);
        min_low = min_low.min(bars[i].low);
    }
    let is_high = bars[center].high > max_high;
    let is_low = bars[center].low < min_low;
    (is_high, is_low)
}

// ---------------------------------------------------------------------------
// Resampling (1m -> higher timeframe) for strategies that need it.
// Bars are accumulated in a VecDeque and emitted when the higher-timeframe
// bucket changes.  This is used by ICT Silver Bullet, Casper, and Rosato.
// ---------------------------------------------------------------------------

pub fn timeframe_minutes(tf: &str) -> u32 {
    let s = tf.trim().to_lowercase();
    let num: String = s.chars().take_while(|c| c.is_ascii_digit()).collect();
    let n: u32 = num.parse().unwrap_or(1);
    if s.contains('h') {
        n * 60
    } else {
        n
    }
}

pub fn bucket_start(timestamp_ns: i64, minutes: u32) -> i64 {
    let secs = timestamp_ns / 1_000_000_000;
    let bucket = (secs / 60) / minutes.max(1) as i64 * minutes.max(1) as i64;
    bucket * 60 * 1_000_000_000
}

#[derive(Debug, Clone, Default)]
pub struct ResampledBar {
    pub timestamp_ns: i64,
    pub open: f64,
    pub high: f64,
    pub low: f64,
    pub close: f64,
    pub volume: u64,
}

pub struct Resampler {
    minutes: u32,
    current: Option<ResampledBar>,
}

impl Resampler {
    pub fn new(tf: &str) -> Self {
        Resampler { minutes: timeframe_minutes(tf), current: None }
    }

    pub fn feed(&mut self, bar: &Bar) -> Option<ResampledBar> {
        let bucket = bucket_start(bar.timestamp_ns, self.minutes);
        match &mut self.current {
            Some(c) if c.timestamp_ns == bucket => {
                c.high = c.high.max(bar.high);
                c.low = c.low.min(bar.low);
                c.close = bar.close;
                c.volume += bar.volume;
                None
            }
            Some(c) => {
                let finished = c.clone();
                *c = ResampledBar {
                    timestamp_ns: bucket,
                    open: bar.open,
                    high: bar.high,
                    low: bar.low,
                    close: bar.close,
                    volume: bar.volume,
                };
                Some(finished)
            }
            None => {
                self.current = Some(ResampledBar {
                    timestamp_ns: bucket,
                    open: bar.open,
                    high: bar.high,
                    low: bar.low,
                    close: bar.close,
                    volume: bar.volume,
                });
                None
            }
        }
    }

    pub fn flush(&mut self) -> Option<ResampledBar> {
        self.current.take()
    }
}

// ---------------------------------------------------------------------------
// Filter evaluation
// ---------------------------------------------------------------------------

pub fn evaluate_filter(cfg: &StrategyConfig, bars: &[Bar]) -> Option<bool> {
    let filter = match cfg.filter.as_deref() {
        Some(f) => f,
        None => return Some(true),
    };
    if filter.eq_ignore_ascii_case("none") {
        return Some(true);
    }
    let close: Vec<f64> = bars.iter().map(|b| b.close).collect();
    let prev_close = *close.last()?;
    let _atr_val = atr_from_series(
        &bars.iter().map(|b| b.high).collect::<Vec<_>>(),
        &bars.iter().map(|b| b.low).collect::<Vec<_>>(),
        &close,
        cfg.atr_length,
    );

    match filter {
        "adx_gt_20" | "adx_gt_25" | "adx_gt_30" | "adx_gt_35" | "adx_gt_40" |
        "adx_lt_20" | "adx_lt_30" | "adx_lt_40" => {
            let v: f64 = filter.strip_prefix("adx_").and_then(|s| s.strip_prefix("gt_").or_else(|| s.strip_prefix("lt_")))?.parse().ok()?;
            let adx_val = adx(bars, 14)?;
            if filter.contains("_gt_") {
                Some(adx_val > v)
            } else {
                Some(adx_val < v)
            }
        }
        "rsi_lt_70" | "rsi_gt_30" | "rsi_lt_80" | "rsi_gt_40" | "rsi_lt_60" | "rsi_gt_50" | "rsi_gt_70" => {
            let v: f64 = if filter.contains("_lt_") {
                filter.strip_prefix("rsi_lt_")?.parse().ok()?
            } else {
                filter.strip_prefix("rsi_gt_")?.parse().ok()?
            };
            let rsi_val = rsi(&close, 14)?;
            if filter.contains("_lt_") {
                Some(rsi_val < v)
            } else {
                Some(rsi_val > v)
            }
        }
        "price_gt_vwap" => Some(prev_close > vwap_up_to(bars, bars.len() - 1)?),
        "price_lt_vwap" => Some(prev_close < vwap_up_to(bars, bars.len() - 1)?),
        "ema20_gt_ema50" | "ema20_gt_sma50" => {
            let e20 = ema(&close, 20)?;
            let s50 = sma(&close, 50)?;
            Some(e20 > s50)
        }
        "ema9_lt_ema20" => Some(ema(&close, 9)? < ema(&close, 20)?),
        "ema9_lt_ema21" => Some(ema(&close, 9)? < ema(&close, 21)?),
        "volume_gt_sma20" => Some((bars.last()?.volume as f64) > volume_sma(&bars.iter().map(|b| b.volume).collect::<Vec<_>>(), 20)?),
        "volume_gt_sma50" => Some((bars.last()?.volume as f64) > volume_sma(&bars.iter().map(|b| b.volume).collect::<Vec<_>>(), 50)?),
        "volume_gt_sma100" => Some((bars.last()?.volume as f64) > volume_sma(&bars.iter().map(|b| b.volume).collect::<Vec<_>>(), 100)?),
        "macd_hist_gt_0" => Some(macd_hist(&close, 12, 26, 9)? > 0.0),
        "macd_hist_lt_0" => Some(macd_hist(&close, 12, 26, 9)? < 0.0),
        "price_gt_sma200" => Some(prev_close > sma(&close, 200)?),
        "price_lt_sma200" => Some(prev_close < sma(&close, 200)?),
        "price_gt_bb_upper" => {
            let (upper, _, _) = bollinger(&close, 20, 2.0)?;
            Some(prev_close > upper)
        }
        "price_gt_kc_lower" => {
            let (_, _, lower) = keltner(bars, 20, 1.5)?;
            Some(prev_close > lower)
        }
        "price_lt_kc_upper" => {
            let (upper, _, _) = keltner(bars, 20, 1.5)?;
            Some(prev_close < upper)
        }
        "macd_signal_cross_up" => {
            let cur = macd_hist(&close, 12, 26, 9)?;
            let prev = macd_hist(&close[..close.len() - 1], 12, 26, 9)?;
            Some(cur > 0.0 && prev <= 0.0)
        }
        "ema20_gt_sma200" => Some(ema(&close, 20)? > sma(&close, 200)?),
        "ema21_gt_sma200" => Some(ema(&close, 21)? > sma(&close, 200)?),
        "price_gt_ema21" => Some(prev_close > ema(&close, 21)?),
        "price_lt_ema21" => Some(prev_close < ema(&close, 21)?),
        "price_gt_kc_upper" => {
            let (upper, _, _) = keltner(bars, 20, 1.5)?;
            Some(prev_close > upper)
        }
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

pub fn build_entry_signal(direction: i8, entry_price: f64) -> Signal {
    Signal {
        direction,
        entry_price,
        stop_loss: 0.0,
        take_profit: 0.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bar(ts: i64, o: f64, h: f64, l: f64, c: f64, v: u64) -> Bar {
        Bar { timestamp_ns: ts, open: o, high: h, low: l, close: c, volume: v }
    }

    #[test]
    fn test_sma_ema() {
        let mut series = BarSeries::new(100);
        for i in 1..=30 {
            series.push(bar(i as i64 * 60 * 1_000_000_000, 1.0, 2.0, 0.5, i as f64, 100));
        }
        let close: Vec<f64> = series.bars.iter().map(|b| b.close).collect();
        assert!(sma(&close, 20).is_some());
        assert!(ema(&close, 20).is_some());
    }

    #[test]
    fn test_session_detection() {
        // 2024-01-15 14:30 UTC = 09:30 NY (winter, UTC-5)
        let ts = 1705329000i64 * 1_000_000_000;
        assert!(in_session(ts, "09:30", "16:00"));
        assert!(!in_session(ts, "12:00", "16:00"));
    }
}
