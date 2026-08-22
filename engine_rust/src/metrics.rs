// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Basic quant metrics: total return, win rate, profit factor, max drawdown, Sharpe.
// 2026-08-20  kilo
//   - Added Sortino, Calmar, avg trade, avg per day/week, hold-time percentiles,
//     and failure count; switched to daily-return annualization using timezone.
// 2026-08-20  kilo
//   - Added combine pass/fail statistics and time-to-pass metrics for Topstep
//     reset-on-failure simulations.
// WHY: Full quant reporting suite required for strategy evaluation.

use std::collections::BTreeMap;

use chrono::{DateTime, Datelike};
use chrono_tz::Tz;

use crate::engine::Trade;

#[derive(Debug, Clone, Default)]
pub struct Summary {
    pub total_return: f64,
    pub n_trades: usize,
    pub win_rate: f64,
    pub profit_factor: f64,
    pub max_drawdown: f64,
    pub sharpe: f64,
    pub sortino: f64,
    pub calmar: f64,
    pub avg_trade: f64,
    pub avg_per_day: f64,
    pub avg_per_week: f64,
    pub hold_time_p50: f64,
    pub hold_time_p90: f64,
    pub hold_time_p95: f64,
    pub failures: u32,
    // Extended Topstep/reporting fields populated by the engine after simulation.
    pub final_equity: f64,
    pub total_pnl: f64,
    pub executed_trades: usize,
    pub daily_limit_hits: u32,
    pub trailing_limit_hits: u32,
    pub account_failed: bool,
    pub account_blowups: u32,
    pub profit_target_reached: bool,
    // Combine pass/fail and time-to-pass statistics (Topstep reset-on-failure mode).
    pub passed_combines: u32,
    pub failed_combines: u32,
    pub combine_pass_rate: f64,
    pub avg_days_to_pass: f64,
    pub median_days_to_pass: f64,
    pub min_days_to_pass: u32,
    pub max_days_to_pass: u32,
    pub days_to_pass_p50: f64,
    pub days_to_pass_p75: f64,
    pub days_to_pass_p90: f64,
    pub days_to_pass_p95: f64,
    pub days_to_pass_p99: f64,
    pub avg_trades_to_pass: f64,
    pub median_trades_to_pass: f64,
}

impl Summary {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        starting_cash: f64,
        tz: Tz,
        timestamps: &[i64],
        equity: &[f64],
        trades: &[Trade],
        failures: u32,
        days_to_pass: &[u32],
        trades_to_pass: &[u32],
    ) -> Self {
        let final_equity = equity.last().copied().unwrap_or(starting_cash);
        let total_return = if starting_cash > 0.0 {
            (final_equity - starting_cash) / starting_cash
        } else {
            0.0
        };

        let n_trades = trades.len();
        let wins: Vec<f64> = trades.iter().filter(|t| t.pnl > 0.0).map(|t| t.pnl).collect();
        let losses: Vec<f64> = trades
            .iter()
            .filter(|t| t.pnl <= 0.0)
            .map(|t| t.pnl)
            .collect();

        let win_rate = if n_trades > 0 {
            wins.len() as f64 / n_trades as f64
        } else {
            0.0
        };

        let gross_profit = wins.iter().sum::<f64>();
        let gross_loss = losses.iter().sum::<f64>().abs();
        let profit_factor = if gross_loss > 0.0 {
            gross_profit / gross_loss
        } else if gross_profit > 0.0 {
            f64::INFINITY
        } else {
            0.0
        };

        let max_drawdown = max_drawdown_pct(equity);
        let daily_rets = daily_returns(timestamps, equity, tz);
        let sharpe = annualized_sharpe(&daily_rets);
        let sortino = annualized_sortino(&daily_rets);
        let calmar = calmar_ratio(total_return, timestamps, tz, max_drawdown);

        let avg_trade = if n_trades > 0 {
            trades.iter().map(|t| t.pnl).sum::<f64>() / n_trades as f64
        } else {
            0.0
        };

        let avg_per_day = avg_pnl_by_period(trades, tz, Period::Day);
        let avg_per_week = avg_pnl_by_period(trades, tz, Period::Week);
        let (hold_time_p50, hold_time_p90, hold_time_p95) = hold_time_percentiles(trades);

        let mut days_to_pass_sorted = days_to_pass.to_vec();
        days_to_pass_sorted.sort_unstable();
        let mut trades_to_pass_sorted = trades_to_pass.to_vec();
        trades_to_pass_sorted.sort_unstable();

        let passed_combines = days_to_pass_sorted.len() as u32;
        let combine_pass_rate = 0.0;
        let avg_days_to_pass = u32_mean(&days_to_pass_sorted);
        let median_days_to_pass = u32_percentile(&days_to_pass_sorted, 0.50);
        let min_days_to_pass = days_to_pass_sorted.first().copied().unwrap_or(0);
        let max_days_to_pass = days_to_pass_sorted.last().copied().unwrap_or(0);
        let days_to_pass_p50 = u32_percentile(&days_to_pass_sorted, 0.50);
        let days_to_pass_p75 = u32_percentile(&days_to_pass_sorted, 0.75);
        let days_to_pass_p90 = u32_percentile(&days_to_pass_sorted, 0.90);
        let days_to_pass_p95 = u32_percentile(&days_to_pass_sorted, 0.95);
        let days_to_pass_p99 = u32_percentile(&days_to_pass_sorted, 0.99);
        let avg_trades_to_pass = u32_mean(&trades_to_pass_sorted);
        let median_trades_to_pass = u32_percentile(&trades_to_pass_sorted, 0.50);

        Summary {
            total_return,
            n_trades,
            win_rate,
            profit_factor,
            max_drawdown,
            sharpe,
            sortino,
            calmar,
            avg_trade,
            avg_per_day,
            avg_per_week,
            hold_time_p50,
            hold_time_p90,
            hold_time_p95,
            failures,
            final_equity: 0.0,
            total_pnl: 0.0,
            executed_trades: 0,
            daily_limit_hits: 0,
            trailing_limit_hits: 0,
            account_failed: false,
            account_blowups: 0,
            profit_target_reached: false,
            passed_combines,
            failed_combines: 0,
            combine_pass_rate,
            avg_days_to_pass,
            median_days_to_pass,
            min_days_to_pass,
            max_days_to_pass,
            days_to_pass_p50,
            days_to_pass_p75,
            days_to_pass_p90,
            days_to_pass_p95,
            days_to_pass_p99,
            avg_trades_to_pass,
            median_trades_to_pass,
        }
    }
}

fn max_drawdown_pct(equity: &[f64]) -> f64 {
    let mut peak = f64::NEG_INFINITY;
    let mut dd = 0.0;
    for &val in equity {
        if val > peak {
            peak = val;
        }
        let drawdown = if peak > 0.0 { (peak - val) / peak } else { 0.0 };
        if drawdown > dd {
            dd = drawdown;
        }
    }
    dd
}

fn to_local(timestamp_ns: i64, tz: Tz) -> DateTime<Tz> {
    let secs = timestamp_ns / 1_000_000_000;
    let nanos = (timestamp_ns % 1_000_000_000) as u32;
    DateTime::from_timestamp(secs, nanos)
        .unwrap_or_else(|| DateTime::UNIX_EPOCH)
        .with_timezone(&tz)
}

fn daily_returns(timestamps: &[i64], equity: &[f64], tz: Tz) -> Vec<f64> {
    let n = timestamps.len().min(equity.len());
    if n < 2 {
        return Vec::new();
    }
    let mut daily: BTreeMap<chrono::NaiveDate, f64> = BTreeMap::new();
    for i in 0..n {
        let local = to_local(timestamps[i], tz);
        let date = local.date_naive();
        // Keep the last equity value observed for each trading day.
        daily.insert(date, equity[i]);
    }
    let values: Vec<f64> = daily.into_values().collect();
    if values.len() < 2 {
        return Vec::new();
    }
    values
        .windows(2)
        .filter_map(|w| if w[0] != 0.0 { Some((w[1] - w[0]) / w[0]) } else { None })
        .collect()
}

fn mean(rets: &[f64]) -> f64 {
    if rets.is_empty() {
        0.0
    } else {
        rets.iter().sum::<f64>() / rets.len() as f64
    }
}

fn sample_std(rets: &[f64], mean_val: f64) -> f64 {
    if rets.len() < 2 {
        return 0.0;
    }
    let var = rets
        .iter()
        .map(|r| (r - mean_val).powi(2))
        .sum::<f64>()
        / (rets.len() - 1) as f64;
    var.sqrt()
}

const TRADING_DAYS_PER_YEAR: f64 = 252.0;

fn annualized_sharpe(rets: &[f64]) -> f64 {
    if rets.len() < 2 {
        return 0.0;
    }
    let m = mean(rets);
    let std = sample_std(rets, m);
    if std == 0.0 {
        0.0
    } else {
        m / std * TRADING_DAYS_PER_YEAR.sqrt()
    }
}

fn annualized_sortino(rets: &[f64]) -> f64 {
    if rets.is_empty() {
        return 0.0;
    }
    let m = mean(rets);
    let downside: Vec<f64> = rets.iter().filter(|&&r| r < 0.0).copied().collect();
    if downside.len() < 2 {
        return 0.0;
    }
    let dm = mean(&downside);
    let dstd = sample_std(&downside, dm);
    if dstd == 0.0 {
        0.0
    } else {
        m / dstd * TRADING_DAYS_PER_YEAR.sqrt()
    }
}

fn duration_years(timestamps: &[i64]) -> f64 {
    if timestamps.len() < 2 {
        return 0.0;
    }
    let first = timestamps[0];
    let last = timestamps[timestamps.len() - 1];
    let ns = (last - first) as f64;
    ns / (365.25 * 24.0 * 60.0 * 60.0 * 1_000_000_000.0)
}

fn calmar_ratio(total_return: f64, timestamps: &[i64], tz: Tz, max_dd: f64) -> f64 {
    if max_dd <= 0.0 {
        return 0.0;
    }
    let years = duration_years(timestamps);
    // Use number of distinct trading days to infer annualization if timestamps are sparse.
    let daily: std::collections::HashSet<chrono::NaiveDate> = timestamps
        .iter()
        .map(|&ts| to_local(ts, tz).date_naive())
        .collect();
    let n_days = daily.len().max(1);
    let annual_return = if years > 0.0 {
        (1.0 + total_return).powf(1.0 / years) - 1.0
    } else if n_days > 1 {
        total_return * (252.0 / n_days as f64)
    } else {
        total_return
    };
    annual_return / max_dd
}

#[derive(Clone, Copy)]
enum Period {
    Day,
    Week,
}

fn avg_pnl_by_period(trades: &[Trade], tz: Tz, period: Period) -> f64 {
    if trades.is_empty() {
        return 0.0;
    }
    let mut groups: BTreeMap<String, f64> = BTreeMap::new();
    for trade in trades {
        let local = to_local(trade.exit_ts, tz);
        let key = match period {
            Period::Day => local.date_naive().to_string(),
            Period::Week => format!("{}-W{:02}", local.year(), local.iso_week().week()),
        };
        *groups.entry(key).or_insert(0.0) += trade.pnl;
    }
    groups.values().sum::<f64>() / groups.len() as f64
}

fn hold_time_percentiles(trades: &[Trade]) -> (f64, f64, f64) {
    if trades.is_empty() {
        return (0.0, 0.0, 0.0);
    }
    let mut durations: Vec<f64> = trades
        .iter()
        .map(|t| ((t.exit_ts - t.entry_ts) as f64) / 1_000_000_000.0)
        .collect();
    durations.sort_by(|a, b| a.partial_cmp(b).unwrap());
    (
        percentile(&durations, 0.50),
        percentile(&durations, 0.90),
        percentile(&durations, 0.95),
    )
}

fn percentile(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    let idx = p * (sorted.len() - 1) as f64;
    let lower = idx.floor() as usize;
    let upper = idx.ceil() as usize;
    if lower == upper || upper >= sorted.len() {
        sorted[lower.min(sorted.len() - 1)]
    } else {
        let frac = idx - lower as f64;
        sorted[lower] + frac * (sorted[upper] - sorted[lower])
    }
}

fn u32_mean(values: &[u32]) -> f64 {
    if values.is_empty() {
        0.0
    } else {
        values.iter().map(|&v| v as f64).sum::<f64>() / values.len() as f64
    }
}

fn u32_percentile(sorted: &[u32], p: f64) -> f64 {
    if sorted.is_empty() {
        return f64::NAN;
    }
    let idx = p * (sorted.len() - 1) as f64;
    let lower = idx.floor() as usize;
    let upper = idx.ceil() as usize;
    if lower == upper || upper >= sorted.len() {
        sorted[lower.min(sorted.len() - 1)] as f64
    } else {
        let frac = idx - lower as f64;
        sorted[lower] as f64 + frac * (sorted[upper] as f64 - sorted[lower] as f64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_trade(entry_ts: i64, exit_ts: i64, pnl: f64) -> Trade {
        Trade {
            entry_ts,
            exit_ts,
            entry_price: 0.0,
            exit_price: 0.0,
            direction: 1,
            qty: 1,
            pnl,
        }
    }

    #[test]
    fn test_metrics_basic() {
        // Daily timestamps to produce one daily return.
        let timestamps = vec![
            0,
            86_400_000_000_000i64,
            2 * 86_400_000_000_000,
            3 * 86_400_000_000_000,
        ];
        let equity = vec![100.0, 110.0, 105.0, 120.0];
        let trades = vec![
            make_trade(0, 86_400_000_000_000, 10.0),
            make_trade(86_400_000_000_000, 2 * 86_400_000_000_000, 5.0),
        ];
        let s = Summary::new(100.0, Tz::UTC, &timestamps, &equity, &trades, 0, &[], &[]);
        assert!((s.total_return - 0.20).abs() < 1e-9);
        assert_eq!(s.n_trades, 2);
        assert!((s.win_rate - 1.0).abs() < 1e-9);
        assert!(s.profit_factor.is_infinite());
        assert!(s.sharpe > 0.0);
        assert!(s.sortino >= 0.0);
        assert!(s.calmar > 0.0);
        assert!(s.avg_trade > 0.0);
        assert!(s.hold_time_p50 > 0.0);
    }

    #[test]
    fn test_max_drawdown() {
        let equity = vec![100.0, 110.0, 90.0, 95.0];
        assert!((max_drawdown_pct(&equity) - 0.1818181818).abs() < 1e-6);
    }

    #[test]
    fn test_hold_time_percentiles() {
        let trades = vec![
            make_trade(0, 1_000_000_000, 1.0),
            make_trade(0, 2_000_000_000, 1.0),
            make_trade(0, 10_000_000_000, 1.0),
        ];
        let (p50, p90, p95) = hold_time_percentiles(&trades);
        assert!((p50 - 2.0).abs() < 1e-9);
        assert!((p90 - 8.4).abs() < 1e-9);
        assert!((p95 - 9.2).abs() < 1e-9);
    }
}
