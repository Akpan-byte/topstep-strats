// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Shared closed-bar indicator math and NY-session utilities for Paper-2 engines.
//   - Mirrors topstep_strats/strategies/paper2_matrix.py helpers using only
//     data available up to the previous closed bar (shift(1) semantics).
// WHY: Keep the 9 Rust C ABI plugins DRY and aligned with the Python reference.

use engine_abi::Bar;
use chrono::{DateTime, Datelike, Timelike, Utc};
use chrono_tz::America::New_York;
use serde_json::Value;

// -----------------------------------------------------------------------------
// Time / session helpers (America/New_York, DST-aware)
// -----------------------------------------------------------------------------

/// Convert a "HH:MM" string to seconds since midnight.
pub fn parse_time_to_seconds(time_str: &str) -> i64 {
    let parts: Vec<&str> = time_str.split(':').collect();
    let h: i64 = parts.first().and_then(|s| s.parse().ok()).unwrap_or(0);
    let m: i64 = parts.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    h * 3600 + m * 60
}

/// Return the local time-of-day in seconds for a UTC nanosecond timestamp.
pub fn local_time_seconds(timestamp_ns: i64) -> i64 {
    let dt = DateTime::<Utc>::from_timestamp_nanos(timestamp_ns);
    let ny = dt.with_timezone(&New_York);
    ny.hour() as i64 * 3600 + ny.minute() as i64 * 60 + ny.second() as i64
}

/// Return a day-number for a UTC nanosecond timestamp in America/New_York.
pub fn date_day(timestamp_ns: i64) -> i32 {
    let dt = DateTime::<Utc>::from_timestamp_nanos(timestamp_ns);
    let ny = dt.with_timezone(&New_York);
    // Days since an arbitrary epoch (year*10000 + month*100 + day).
    ny.year() * 10_000 + ny.month() as i32 * 100 + ny.day() as i32
}

/// True if local time of day falls in [start_sec, end_sec] (inclusive).
/// Handles overnight sessions where start_sec > end_sec.
pub fn in_session(timestamp_ns: i64, start_sec: i64, end_sec: i64) -> bool {
    let tod = local_time_seconds(timestamp_ns);
    if start_sec <= end_sec {
        tod >= start_sec && tod <= end_sec
    } else {
        tod >= start_sec || tod <= end_sec
    }
}

// -----------------------------------------------------------------------------
// Indicator helpers
// -----------------------------------------------------------------------------

/// Simple moving average using only closed bars up to index `i-1`.
pub fn sma(values: &[f64], length: usize) -> Vec<Option<f64>> {
    let n = values.len();
    let mut out = vec![None; n];
    if length == 0 || n < length + 1 {
        return out;
    }
    let mut sum = values[..length].iter().sum::<f64>();
    out[length] = Some(sum / length as f64);
    for i in length + 1..n {
        sum += values[i - 1] - values[i - 1 - length];
        out[i] = Some(sum / length as f64);
    }
    out
}

/// Exponential moving average (pandas ewm(span, adjust=false), shifted by 1).
pub fn ema(values: &[f64], span: usize) -> Vec<Option<f64>> {
    let n = values.len();
    let mut out = vec![None; n];
    if span == 0 || n < span + 1 {
        return out;
    }
    let alpha = 2.0 / (span as f64 + 1.0);
    let mut ema = values[0];
    for i in 1..span {
        ema = values[i] * alpha + ema * (1.0 - alpha);
    }
    out[span] = Some(ema);
    for i in span + 1..n {
        ema = values[i - 1] * alpha + ema * (1.0 - alpha);
        out[i] = Some(ema);
    }
    out
}

pub fn true_range(bars: &[Bar]) -> Vec<f64> {
    let mut out = Vec::with_capacity(bars.len());
    for (i, b) in bars.iter().enumerate() {
        if i == 0 {
            out.push(b.high - b.low);
        } else {
            let prev_close = bars[i - 1].close;
            let tr = (b.high - b.low)
                .max((b.high - prev_close).abs())
                .max((b.low - prev_close).abs());
            out.push(tr);
        }
    }
    out
}

pub fn atr(bars: &[Bar], length: usize) -> Vec<Option<f64>> {
    let tr = true_range(bars);
    sma(&tr, length)
}

/// Wilder-style RSI using SMA of gains/losses (matches the simplified Python impl).
pub fn rsi(closes: &[f64], length: usize) -> Vec<Option<f64>> {
    let n = closes.len();
    let mut out = vec![None; n];
    if length == 0 || n < length + 2 {
        return out;
    }
    let mut gains = vec![0.0; n];
    let mut losses = vec![0.0; n];
    for i in 1..n {
        let diff = closes[i] - closes[i - 1];
        if diff > 0.0 {
            gains[i] = diff;
        } else {
            losses[i] = -diff;
        }
    }
    let gain_sma = sma(&gains, length);
    let loss_sma = sma(&losses, length);
    for i in 0..n {
        out[i] = match (gain_sma[i], loss_sma[i]) {
            (Some(g), Some(l)) => {
                if l == 0.0 {
                    Some(100.0)
                } else if g == 0.0 {
                    Some(0.0)
                } else {
                    let rs = g / l;
                    Some(100.0 - (100.0 / (1.0 + rs)))
                }
            }
            _ => None,
        };
    }
    out
}

pub fn macd_histogram(closes: &[f64], fast: usize, slow: usize, signal: usize) -> Vec<Option<f64>> {
    let ema_fast = ema(closes, fast);
    let ema_slow = ema(closes, slow);
    let n = closes.len();
    let mut macd = vec![None; n];
    for i in 0..n {
        if let (Some(f), Some(s)) = (ema_fast[i], ema_slow[i]) {
            macd[i] = Some(f - s);
        }
    }
    // pandas: signal_line = macd.ewm(span=signal).mean().shift(1)
    // Extract macd as f64 with NaNs for SMA computation.
    let macd_vals: Vec<f64> = macd.iter().map(|x| x.unwrap_or(f64::NAN)).collect();
    let signal_line = ema(&macd_vals, signal);
    let mut out = vec![None; n];
    for i in 0..n {
        if let (Some(m), Some(s)) = (macd[i], signal_line[i]) {
            out[i] = Some(m - s);
        }
    }
    out
}

/// Daily-anchored VWAP using only closed bars up to index `i-1`.
pub fn vwap(bars: &[Bar]) -> Vec<Option<f64>> {
    let n = bars.len();
    let mut out = vec![None; n];
    if n == 0 {
        return out;
    }
    let mut day_start = 0usize;
    let mut cum_typ_vol = 0.0;
    let mut cum_vol = 0.0f64;
    for i in 1..n {
        if date_day(bars[i].timestamp_ns) != date_day(bars[i - 1].timestamp_ns) {
            day_start = i;
            cum_typ_vol = 0.0;
            cum_vol = 0.0;
        }
        if i > day_start {
            // Value at i uses bars up to i-1 within the same local day.
            out[i] = if cum_vol > 0.0 {
                Some(cum_typ_vol / cum_vol)
            } else {
                None
            };
        }
        let typical = (bars[i].high + bars[i].low + bars[i].close) / 3.0;
        let vol = bars[i].volume as f64;
        cum_typ_vol += typical * vol;
        cum_vol += vol;
    }
    out
}

pub fn vwap_std(bars: &[Bar], length: usize) -> Vec<Option<f64>> {
    let vwap = vwap(bars);
    let n = bars.len();
    let mut out = vec![None; n];
    if length == 0 || n < length + 1 {
        return out;
    }
    for i in length..n {
        if vwap[i].is_none() {
            continue;
        }
        let mut sum_sq = 0.0;
        let mut count = 0usize;
        for j in (i - length)..i {
            if let Some(v) = vwap[j] {
                let diff = bars[j].close - v;
                sum_sq += diff * diff;
                count += 1;
            }
        }
        if count > 0 {
            out[i] = Some((sum_sq / count as f64).sqrt());
        }
    }
    out
}

pub fn vwap_zscore(bars: &[Bar], length: usize) -> Vec<Option<f64>> {
    let vwap = vwap(bars);
    let std = vwap_std(bars, length);
    let n = bars.len();
    let mut out = vec![None; n];
    for i in 0..n {
        if let (Some(v), Some(s)) = (vwap[i], std[i]) {
            if s != 0.0 {
                out[i] = Some((bars[i].close - v) / s);
            }
        }
    }
    out
}

pub fn volume_sma(bars: &[Bar], length: usize) -> Vec<Option<f64>> {
    let volumes: Vec<f64> = bars.iter().map(|b| b.volume as f64).collect();
    sma(&volumes, length)
}

pub fn volume_percentile(bars: &[Bar], length: usize, pct: f64) -> Vec<Option<f64>> {
    let n = bars.len();
    let mut out = vec![None; n];
    if length == 0 || n < length + 1 {
        return out;
    }
    for i in length..n {
        let mut window: Vec<f64> = bars[i - length..i].iter().map(|b| b.volume as f64).collect();
        if window.is_empty() {
            continue;
        }
        window.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let idx = ((pct / 100.0) * (window.len() - 1) as f64).round() as usize;
        out[i] = Some(window[idx.min(window.len() - 1)]);
    }
    out
}

pub fn bollinger(
    closes: &[f64],
    length: usize,
    std_dev: f64,
) -> (Vec<Option<f64>>, Vec<Option<f64>>, Vec<Option<f64>>) {
    let mid = sma(closes, length);
    let n = closes.len();
    let mut upper = vec![None; n];
    let mut lower = vec![None; n];
    for i in 0..n {
        if let Some(m) = mid[i] {
            let mut sum_sq = 0.0;
            let mut count = 0usize;
            for j in (i.saturating_sub(length))..i {
                let diff = closes[j] - m;
                sum_sq += diff * diff;
                count += 1;
            }
            if count > 0 {
                let std = (sum_sq / count as f64).sqrt();
                upper[i] = Some(m + std_dev * std);
                lower[i] = Some(m - std_dev * std);
            }
        }
    }
    (upper, mid, lower)
}

pub fn keltner(
    bars: &[Bar],
    length: usize,
    mult: f64,
) -> (Vec<Option<f64>>, Vec<Option<f64>>, Vec<Option<f64>>) {
    let closes: Vec<f64> = bars.iter().map(|b| b.close).collect();
    let mid = ema(&closes, length);
    let atr_vals = atr(bars, length);
    let n = bars.len();
    let mut upper = vec![None; n];
    let mut lower = vec![None; n];
    for i in 0..n {
        if let (Some(m), Some(a)) = (mid[i], atr_vals[i]) {
            upper[i] = Some(m + mult * a);
            lower[i] = Some(m - mult * a);
        }
    }
    (upper, mid, lower)
}

/// ADX using the simplified Python formula (rolling means, not Wilder smoothing).
pub fn adx(bars: &[Bar], length: usize) -> Vec<f64> {
    let n = bars.len();
    let mut out = vec![0.0; n];
    if length == 0 || n < length + 2 {
        return out;
    }
    let mut plus_dm = vec![0.0; n];
    let mut minus_dm = vec![0.0; n];
    for i in 1..n {
        let up_move = bars[i].high - bars[i - 1].high;
        let down_move = bars[i - 1].low - bars[i].low;
        if up_move > down_move && up_move > 0.0 {
            plus_dm[i] = up_move;
        }
        if down_move > up_move && down_move > 0.0 {
            minus_dm[i] = down_move;
        }
    }
    let tr = true_range(bars);
    let atr_vals = sma(&tr, length);
    let plus_di: Vec<Option<f64>> = plus_dm
        .iter()
        .zip(atr_vals.iter())
        .map(|(dm, atr)| {
            atr.map(|a| if a == 0.0 { 0.0 } else { 100.0 * dm / a })
        })
        .collect();
    let minus_di: Vec<Option<f64>> = minus_dm
        .iter()
        .zip(atr_vals.iter())
        .map(|(dm, atr)| {
            atr.map(|a| if a == 0.0 { 0.0 } else { 100.0 * dm / a })
        })
        .collect();
    let mut dx = vec![None; n];
    for i in 0..n {
        if let (Some(p), Some(m)) = (plus_di[i], minus_di[i]) {
            let denom = p + m;
            if denom != 0.0 {
                dx[i] = Some((p - m).abs() / denom * 100.0);
            }
        }
    }
    let dx_f64: Vec<f64> = dx.iter().map(|x| x.unwrap_or(f64::NAN)).collect();
    let adx_sma = sma(&dx_f64, length);
    for i in 0..n {
        out[i] = adx_sma[i].unwrap_or(0.0);
    }
    out
}

/// Centered rolling swing highs/lows with shift(1) semantics.
/// swing_high[i] is true if high[i-1] is the maximum of [i-1-lookback, i-1+lookback].
pub fn swing_highs_lows(bars: &[Bar], lookback: usize) -> (Vec<bool>, Vec<bool>) {
    let n = bars.len();
    let mut swing_high = vec![false; n];
    let mut swing_low = vec![false; n];
    if lookback == 0 || n < 2 * lookback + 2 {
        return (swing_high, swing_low);
    }
    for i in lookback + 1..n - lookback + 1 {
        let center = i - 1;
        let start = center.saturating_sub(lookback);
        let end = (center + lookback + 1).min(n);
        let mut max_high = bars[start].high;
        let mut min_low = bars[start].low;
        for j in start..end {
            if bars[j].high > max_high {
                max_high = bars[j].high;
            }
            if bars[j].low < min_low {
                min_low = bars[j].low;
            }
        }
        swing_high[i] = bars[center].high == max_high;
        swing_low[i] = bars[center].low == min_low;
    }
    (swing_high, swing_low)
}

/// Approximate rolling Volume Point of Control using 12 bins over the lookback.
pub fn rolling_poc(bars: &[Bar], lookback: usize) -> Vec<Option<f64>> {
    let n = bars.len();
    let mut out = vec![None; n];
    if lookback == 0 || n < lookback + 1 {
        return out;
    }
    for i in lookback..n {
        let start = i - lookback;
        let mut total_vol = 0.0;
        let mut min_typ = f64::INFINITY;
        let mut max_typ = f64::NEG_INFINITY;
        for j in start..i {
            let typical = (bars[j].high + bars[j].low + bars[j].close) / 3.0;
            total_vol += bars[j].volume as f64;
            if typical < min_typ {
                min_typ = typical;
            }
            if typical > max_typ {
                max_typ = typical;
            }
        }
        if total_vol <= 0.0 || min_typ == max_typ {
            out[i] = Some(max_typ);
            continue;
        }
        let mut bins = [0.0; 12];
        let step = (max_typ - min_typ) / 12.0;
        for j in start..i {
            let typical = (bars[j].high + bars[j].low + bars[j].close) / 3.0;
            let mut idx = ((typical - min_typ) / step).floor() as isize;
            if idx < 0 {
                idx = 0;
            }
            if idx > 11 {
                idx = 11;
            }
            bins[idx as usize] += bars[j].volume as f64;
        }
        let mut best_idx = 0usize;
        let mut best_vol = bins[0];
        for (idx, &vol) in bins.iter().enumerate() {
            if vol > best_vol {
                best_vol = vol;
                best_idx = idx;
            }
        }
        let bin_low = min_typ + step * best_idx as f64;
        let bin_high = min_typ + step * (best_idx as f64 + 1.0);
        out[i] = Some((bin_low + bin_high) / 2.0);
    }
    out
}

// -----------------------------------------------------------------------------
// Filter evaluation
// -----------------------------------------------------------------------------

pub fn evaluate_filter(filter: Option<&str>, bars: &[Bar]) -> Vec<bool> {
    let n = bars.len();
    let all_true = vec![true; n];
    let expr = match filter {
        None | Some("none") => return all_true,
        Some(s) => s,
    };
    let closes: Vec<f64> = bars.iter().map(|b| b.close).collect();
    match expr {
        "adx_gt_20" => adx(bars, 14).iter().map(|&v| v > 20.0).collect(),
        "adx_gt_25" => adx(bars, 14).iter().map(|&v| v > 25.0).collect(),
        "adx_gt_30" => adx(bars, 14).iter().map(|&v| v > 30.0).collect(),
        "adx_gt_35" => adx(bars, 14).iter().map(|&v| v > 35.0).collect(),
        "adx_gt_40" => adx(bars, 14).iter().map(|&v| v > 40.0).collect(),
        "rsi_lt_70" => rsi(&closes, 14).iter().map(|&o| o.map(|v| v < 70.0).unwrap_or(false)).collect(),
        "rsi_gt_30" => rsi(&closes, 14).iter().map(|&o| o.map(|v| v > 30.0).unwrap_or(false)).collect(),
        "rsi_lt_80" => rsi(&closes, 14).iter().map(|&o| o.map(|v| v < 80.0).unwrap_or(false)).collect(),
        "rsi_gt_50" => rsi(&closes, 14).iter().map(|&o| o.map(|v| v > 50.0).unwrap_or(false)).collect(),
        "rsi_lt_60" => rsi(&closes, 14).iter().map(|&o| o.map(|v| v < 60.0).unwrap_or(false)).collect(),
        "price_gt_vwap" => {
            let v = vwap(bars);
            (0..n).map(|i| v[i].map(|x| closes[i] > x).unwrap_or(false)).collect()
        }
        "price_lt_vwap" => {
            let v = vwap(bars);
            (0..n).map(|i| v[i].map(|x| closes[i] < x).unwrap_or(false)).collect()
        }
        "ema9_gt_ema21" => {
            let e9 = ema(&closes, 9);
            let e21 = ema(&closes, 21);
            (0..n)
                .map(|i| e9[i].zip(e21[i]).map(|(a, b)| a > b).unwrap_or(false))
                .collect()
        }
        "ema9_lt_ema21" => {
            let e9 = ema(&closes, 9);
            let e21 = ema(&closes, 21);
            (0..n)
                .map(|i| e9[i].zip(e21[i]).map(|(a, b)| a < b).unwrap_or(false))
                .collect()
        }
        "ema20_gt_sma50" => {
            let e20 = ema(&closes, 20);
            let s50 = sma(&closes, 50);
            (0..n)
                .map(|i| e20[i].zip(s50[i]).map(|(a, b)| a > b).unwrap_or(false))
                .collect()
        }
        "volume_gt_sma20" => {
            let vs = volume_sma(bars, 20);
            (0..n)
                .map(|i| vs[i].map(|x| bars[i].volume as f64 > x).unwrap_or(false))
                .collect()
        }
        "volume_gt_sma50" => {
            let vs = volume_sma(bars, 50);
            (0..n)
                .map(|i| vs[i].map(|x| bars[i].volume as f64 > x).unwrap_or(false))
                .collect()
        }
        "macd_hist_gt_0" => macd_histogram(&closes, 12, 26, 9)
            .iter()
            .map(|&o| o.map(|v| v > 0.0).unwrap_or(false))
            .collect(),
        "macd_hist_lt_0" => macd_histogram(&closes, 12, 26, 9)
            .iter()
            .map(|&o| o.map(|v| v < 0.0).unwrap_or(false))
            .collect(),
        "price_gt_sma200" => {
            let s = sma(&closes, 200);
            (0..n).map(|i| s[i].map(|x| closes[i] > x).unwrap_or(false)).collect()
        }
        "price_lt_sma200" => {
            let s = sma(&closes, 200);
            (0..n).map(|i| s[i].map(|x| closes[i] < x).unwrap_or(false)).collect()
        }
        "price_gt_bb_upper" => {
            let (upper, _, _) = bollinger(&closes, 20, 2.0);
            (0..n).map(|i| upper[i].map(|x| closes[i] > x).unwrap_or(false)).collect()
        }
        "price_lt_bb_lower" => {
            let (_, _, lower) = bollinger(&closes, 20, 2.0);
            (0..n).map(|i| lower[i].map(|x| closes[i] < x).unwrap_or(false)).collect()
        }
        "vwap_zscore_gt_1" => {
            let z = vwap_zscore(bars, 30);
            (0..n).map(|i| z[i].map(|x| x > 1.0).unwrap_or(false)).collect()
        }
        "vwap_zscore_lt_minus1" => {
            let z = vwap_zscore(bars, 30);
            (0..n).map(|i| z[i].map(|x| x < -1.0).unwrap_or(false)).collect()
        }
        _ => {
            eprintln!("paper2_indicators: unknown filter '{expr}', defaulting to true");
            all_true
        }
    }
}

// -----------------------------------------------------------------------------
// Config helpers
// -----------------------------------------------------------------------------

/// Parse a JSON config value or return a default.
pub fn cfg_f64(cfg: &Value, key: &str, default: f64) -> f64 {
    cfg.get(key)
        .and_then(|v| v.as_f64())
        .unwrap_or(default)
}

pub fn cfg_i64(cfg: &Value, key: &str, default: i64) -> i64 {
    cfg.get(key)
        .and_then(|v| v.as_i64())
        .unwrap_or(default)
}

pub fn cfg_usize(cfg: &Value, key: &str, default: usize) -> usize {
    cfg.get(key)
        .and_then(|v| v.as_u64())
        .map(|v| v as usize)
        .unwrap_or(default)
}

pub fn cfg_bool(cfg: &Value, key: &str, default: bool) -> bool {
    cfg.get(key).and_then(|v| v.as_bool()).unwrap_or(default)
}

pub fn cfg_str<'a>(cfg: &'a Value, key: &str, default: &'a str) -> &'a str {
    cfg.get(key).and_then(|v| v.as_str()).unwrap_or(default)
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
    fn test_sma() {
        let prices = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let s = sma(&prices, 2);
        assert!(s[2].unwrap() - 1.5 < 1e-9); // (1+2)/2
        assert!(s[3].unwrap() - 2.5 < 1e-9); // (2+3)/2
    }

    #[test]
    fn test_ema() {
        let prices = vec![1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0];
        let e = ema(&prices, 3);
        assert!(e[3].is_some());
        assert!(e[4].is_some());
    }

    #[test]
    fn test_true_range_and_atr() {
        let bars = vec![
            bar(1, 10.0, 12.0, 9.0, 11.0, 100),
            bar(2, 11.0, 13.0, 10.0, 12.0, 100),
            bar(3, 12.0, 14.0, 11.0, 13.0, 100),
        ];
        let tr = true_range(&bars);
        assert_eq!(tr[0], 3.0);
        assert_eq!(tr[1], 3.0);
        let a = atr(&bars, 2);
        assert!(a[2].is_some());
    }

    #[test]
    fn test_session_times() {
        // Any valid UTC timestamp must map to a value in [0, 86400).
        let ts = 1_756_000_200_000_000_000i64;
        let sec = local_time_seconds(ts);
        assert!(sec >= 0 && sec < 86_400);
    }

    #[test]
    fn test_swing_highs_lows() {
        let bars = vec![
            bar(1, 1.0, 1.0, 1.0, 1.0, 1),
            bar(2, 1.0, 3.0, 1.0, 1.0, 1),
            bar(3, 1.0, 2.0, 1.0, 1.0, 1),
            bar(4, 1.0, 2.0, 1.0, 1.0, 1),
            bar(5, 1.0, 1.0, 1.0, 1.0, 1),
        ];
        let (sh, _sl) = swing_highs_lows(&bars, 1);
        // bar index 1 (high=3) is a swing high; swing_high[2] should flag it.
        assert!(sh[2]);
    }

    #[test]
    fn test_rolling_poc() {
        let bars: Vec<Bar> = (0..20)
            .map(|i| bar(i as i64, i as f64, i as f64 + 1.0, i as f64, i as f64 + 0.5, 100))
            .collect();
        let poc = rolling_poc(&bars, 10);
        assert!(poc[10].is_some());
    }
}
