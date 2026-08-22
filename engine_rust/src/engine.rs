// CHANGE_SUMMARY
// 2026-08-20  kilo
//   - Single-instrument backtest engine: cash, position, market orders, PnL.
// 2026-08-20  kilo
//   - Added Topstep-style risk rules, 1 %-of-equity position sizing,
//     timezone-aware session filtering, limit-order matching, prefilled-trade
//     runner, and extended Topstep reporting fields.
// 2026-08-20  kilo
//   - Added per-combine pass/fail tracking and time-to-pass statistics in
//     run_signals when TopstepConfig reset_on_failure is enabled.
// WHY: Correctness first; support realistic prop-firm constraints and any OHLCV source.

use std::collections::HashSet;
use std::str::FromStr;

use chrono::{DateTime, Datelike, NaiveDate, NaiveTime, Weekday};
use chrono_tz::Tz;

use crate::data::{Bar, ReplayIter};
use crate::metrics::Summary;
use crate::strategy::{Signal, Strategy};

#[derive(Debug, Clone)]
pub struct EngineConfig {
    pub starting_cash: f64,
    /// Per-contract/lot commission applied on entry and exit.
    pub commission_per_contract: f64,
    /// Slippage applied as a price offset on market fills (ticks/points, not pct).
    pub slippage_ticks: f64,
    /// Absolute daily loss limit. 0 disables the rule.
    pub daily_loss_limit: f64,
    /// Absolute trailing drawdown from peak equity. 0 disables the rule.
    pub trailing_drawdown: f64,
    /// If true, reset the account to starting cash after a risk failure.
    pub reset_on_failure: bool,
    /// Multiplier converting price difference to currency PnL per unit.
    pub point_value: f64,
    /// Enable dynamic 1 %-of-equity position sizing per trade.
    pub position_sizing: bool,
    /// IANA timezone name (e.g. "America/New_York"). Empty defaults to UTC.
    pub timezone: String,
    /// Session start local time as "HH:MM". None means no session filter.
    pub session_start: Option<String>,
    /// Session end local time as "HH:MM". None means no session filter.
    pub session_end: Option<String>,
}

impl Default for EngineConfig {
    fn default() -> Self {
        EngineConfig {
            starting_cash: 100_000.0,
            commission_per_contract: 0.0,
            slippage_ticks: 0.0,
            daily_loss_limit: 0.0,
            trailing_drawdown: 0.0,
            reset_on_failure: true,
            point_value: 1.0,
            position_sizing: false,
            timezone: String::new(),
            session_start: None,
            session_end: None,
        }
    }
}

impl EngineConfig {
    fn parse_timezone(&self) -> Tz {
        if self.timezone.is_empty() {
            return Tz::UTC;
        }
        Tz::from_str(&self.timezone).unwrap_or(Tz::UTC)
    }

    fn parse_time(opt: &Option<String>) -> Option<NaiveTime> {
        opt.as_ref()
            .and_then(|s| NaiveTime::parse_from_str(s, "%H:%M").ok())
    }
}

/// Topstep-style risk configuration used by the prefilled-trade runner.
#[derive(Debug, Clone, Copy, Default)]
pub struct TopstepConfig {
    pub enabled: bool,
    /// Absolute daily drawdown limit in account currency.
    pub daily_drawdown_limit: f64,
    /// Absolute trailing drawdown limit in account currency.
    pub trailing_drawdown_limit: f64,
    /// Absolute profit target in account currency.
    pub profit_target: f64,
    /// If true, reset the account to starting cash after a rule breach.
    pub reset_on_failure: bool,
}

/// A pre-completed trade supplied by an external signal generator.
#[derive(Debug, Clone, Copy)]
pub struct PrefilledTrade {
    pub entry_ts: i64,
    pub exit_ts: i64,
    pub direction: i8,
    pub entry_price: f64,
    pub exit_price: f64,
    /// PnL in price points per unit, before point_value and commission.
    pub pnl_points: f64,
}

impl PrefilledTrade {
    /// Realized PnL in account currency.
    ///
    /// ``pnl_points`` is supplied by the external signal generator and is already
    /// signed from the trade's perspective (positive = win, negative = loss), so
    /// we do not multiply by direction again.
    fn realized_pnl(&self, point_value: f64, commission: f64) -> f64 {
        self.pnl_points * point_value - 2.0 * commission
    }
}

impl From<&PrefilledTrade> for Trade {
    fn from(t: &PrefilledTrade) -> Self {
        Trade {
            entry_ts: t.entry_ts,
            exit_ts: t.exit_ts,
            entry_price: t.entry_price,
            exit_price: t.exit_price,
            direction: t.direction,
            qty: 1,
            pnl: 0.0, // filled in by the caller after applying point_value/commission
        }
    }
}

#[derive(Debug, Clone)]
pub struct Fill {
    pub timestamp_ns: i64,
    pub price: f64,
    pub direction: i8,
}

#[derive(Debug, Clone)]
pub struct Trade {
    pub entry_ts: i64,
    pub exit_ts: i64,
    pub entry_price: f64,
    pub exit_price: f64,
    pub direction: i8,
    pub qty: i32,
    pub pnl: f64,
}

pub struct Engine {
    config: EngineConfig,
    tz: Tz,
    session_start: Option<NaiveTime>,
    session_end: Option<NaiveTime>,

    cash: f64,
    position_direction: i8,
    position_qty: i32,
    entry_price: f64,
    entry_ts: i64,

    equity: Vec<f64>,
    timestamps: Vec<i64>,
    trades: Vec<Trade>,
    pending_signal: Option<Signal>,

    high_water_mark: f64,
    daily_start_equity: f64,
    current_day: Option<chrono::NaiveDate>,
    failures: u32,
    daily_limit_hits: u32,
    trailing_limit_hits: u32,
    profit_target_reached: bool,

    prev_equity: f64,
}

impl Engine {
    pub fn new(config: EngineConfig) -> Self {
        let tz = config.parse_timezone();
        let session_start = EngineConfig::parse_time(&config.session_start);
        let session_end = EngineConfig::parse_time(&config.session_end);
        let starting = config.starting_cash;
        Engine {
            config,
            tz,
            session_start,
            session_end,
            cash: starting,
            position_direction: 0,
            position_qty: 0,
            entry_price: 0.0,
            entry_ts: 0,
            equity: vec![starting],
            timestamps: vec![0],
            trades: Vec::new(),
            pending_signal: None,
            high_water_mark: starting,
            daily_start_equity: starting,
            current_day: None,
            failures: 0,
            daily_limit_hits: 0,
            trailing_limit_hits: 0,
            profit_target_reached: false,
            prev_equity: starting,
        }
    }

    /// Run the strategy over the supplied bars and return a summary.
    pub fn run(&mut self, bars: &[Bar], strategy: &mut dyn Strategy) -> Summary {
        let mut iter = ReplayIter::new(bars).peekable();
        while let Some(bar) = iter.next() {
            let local = to_local(bar.timestamp_ns, self.tz);
            let in_session = self.in_session(&local);

            // Roll the trading day before marking this bar.
            self.roll_day(&local);

            // Outside session: cancel pending orders and flatten any position at open.
            if !in_session {
                self.pending_signal = None;
                if self.position_qty != 0 {
                    self.flatten_at(bar.open, bar.timestamp_ns);
                }
            } else if let Some(signal) = self.pending_signal.take() {
                // In session: execute any pending order at this bar's open.
                self.execute_signal(signal, bar);
            }

            // Update equity using this bar's close.
            self.mark_to_market(bar.close);
            self.record_equity_ts(bar.timestamp_ns);

            // Enforce risk rules.
            if self.check_risk(bar.close, bar.timestamp_ns) {
                if !self.config.reset_on_failure {
                    break;
                }
                // After a reset, do not generate a signal this bar.
                self.prev_equity = *self.equity.last().unwrap_or(&self.config.starting_cash);
                continue;
            }

            // Ask the strategy for a decision only during session hours.
            if in_session {
                if let Some(signal) = strategy.on_bar(bar) {
                    if ![-1, 0, 1].contains(&signal.direction) {
                        continue;
                    }
                    if signal.direction != 0 || self.position_qty != 0 {
                        self.pending_signal = Some(signal);
                    }
                }
            }

            self.prev_equity = *self.equity.last().unwrap_or(&self.config.starting_cash);
        }

        // Close any open position at the final close.
        if let Some(last) = bars.last() {
            if self.position_qty != 0 {
                self.flatten_at(last.close, last.timestamp_ns);
            }
        }

        self.build_summary()
    }

    /// Run a pre-completed trade schedule with optional Topstep rule checks.
    ///
    /// Semantics mirror the Python `run_backtest` Topstep path:
    /// * profit_target_reached is recorded but trading continues;
    /// * daily drawdown breaches skip the remainder of the calendar day;
    /// * trailing drawdown breaches either halt the backtest or reset the
    ///   account to starting cash when `reset_on_failure` is enabled;
    /// * cumulative_pnl tracks realized profit/loss across account resets.
    pub fn run_signals(&mut self, trades: &mut [PrefilledTrade], topstep: &TopstepConfig) -> Summary {
        trades.sort_by_key(|t| t.entry_ts);

        let starting = self.config.starting_cash;
        let mut cash = starting;
        let mut equity = vec![cash];
        let mut timestamps = vec![0i64];
        let mut closed_trades: Vec<Trade> = Vec::with_capacity(trades.len());

        let mut high_water = cash;
        let mut daily_start = cash;
        let mut current_day: Option<chrono::NaiveDate> = None;
        let mut daily_limit_hit = false;
        let mut daily_hits = 0u32;
        let mut trailing_hits = 0u32;
        let mut failures = 0u32;
        let mut trailing_blowups = 0u32;
        let mut account_failed = false;
        let mut profit_target_reached = false;
        let mut cumulative_pnl = 0.0;

        // Per-combine state for Topstep reset-on-failure statistics.
        let mut passed_combines = 0u32;
        let mut failed_combines = 0u32;
        let mut current_combine_start_day: Option<NaiveDate> = None;
        let mut current_combine_days: HashSet<NaiveDate> = HashSet::new();
        let mut trades_this_combine = 0u32;
        let mut days_to_pass: Vec<u32> = Vec::new();
        let mut trades_to_pass: Vec<u32> = Vec::new();

        for trade in trades.iter() {
            let local = to_local(trade.entry_ts, self.tz);
            let date = local.date_naive();
            match current_day {
                None => {
                    current_day = Some(date);
                    daily_start = starting;
                    daily_limit_hit = false;
                }
                Some(d) if d != date => {
                    current_day = Some(date);
                    daily_start = cash;
                    daily_limit_hit = false;
                }
                _ => {}
            };

            // Start a new combine on the first trade or after a reset.
            if current_combine_start_day.is_none() {
                current_combine_start_day = Some(date);
                current_combine_days.clear();
                trades_this_combine = 0;
            }

            // Once the trailing limit fails without reset, no further trades execute.
            if account_failed {
                break;
            }

            // Daily limit blocks the rest of the current calendar day.
            if daily_limit_hit {
                continue;
            }

            let pnl = trade.realized_pnl(self.config.point_value, self.config.commission_per_contract);
            let prospective_cash = cash + pnl;
            let prospective_high_water = if prospective_cash > high_water {
                prospective_cash
            } else {
                high_water
            };

            let daily_breached = topstep.enabled
                && topstep.daily_drawdown_limit > 0.0
                && prospective_cash < daily_start - topstep.daily_drawdown_limit;
            let trailing_breached = topstep.enabled
                && topstep.trailing_drawdown_limit > 0.0
                && prospective_cash < prospective_high_water - topstep.trailing_drawdown_limit;

            if daily_breached {
                daily_hits += 1;
                failures += 1;
                daily_limit_hit = true;
                continue;
            }

            if trailing_breached {
                trailing_hits += 1;
                failures += 1;

                if topstep.reset_on_failure {
                    // Trade executes and blows the account; count the loss,
                    // then reset capital and continue with the next trade.
                    cash = prospective_cash;
                    cumulative_pnl += pnl;
                    trailing_blowups += 1;

                    let mut closed = Trade::from(trade);
                    closed.pnl = pnl;
                    closed_trades.push(closed);
                    equity.push(cash);
                    timestamps.push(trade.exit_ts);

                    failed_combines += 1;

                    // Reset capital and combine state.
                    cash = starting;
                    high_water = starting;
                    daily_start = starting;
                    daily_limit_hit = false;
                    current_combine_start_day = None;
                    current_combine_days.clear();
                    trades_this_combine = 0;
                } else {
                    account_failed = true;
                }
                continue;
            }

            // Trade executes normally.
            cash = prospective_cash;
            high_water = prospective_high_water;
            cumulative_pnl += pnl;

            let mut closed = Trade::from(trade);
            closed.pnl = pnl;
            closed_trades.push(closed);
            equity.push(cash);
            timestamps.push(trade.exit_ts);

            current_combine_days.insert(date);
            trades_this_combine += 1;

            if topstep.enabled
                && topstep.profit_target > 0.0
                && cash >= starting + topstep.profit_target
            {
                profit_target_reached = true;
                passed_combines += 1;
                days_to_pass.push(current_combine_days.len() as u32);
                trades_to_pass.push(trades_this_combine);

                // Reset to starting capital and clear combine state.
                cash = starting;
                high_water = starting;
                daily_start = starting;
                daily_limit_hit = false;
                current_combine_start_day = None;
                current_combine_days.clear();
                trades_this_combine = 0;
            }
        }

        let mut summary = Summary::new(
            self.config.starting_cash,
            self.tz,
            &timestamps,
            &equity,
            &closed_trades,
            failures,
            &days_to_pass,
            &trades_to_pass,
        );
        summary.final_equity = cash;
        summary.total_pnl = cumulative_pnl;
        summary.executed_trades = closed_trades.len();
        summary.daily_limit_hits = daily_hits;
        summary.trailing_limit_hits = trailing_hits;
        summary.account_failed = account_failed;
        summary.account_blowups = if account_failed { 1 } else { trailing_blowups };
        summary.profit_target_reached = profit_target_reached;
        summary.passed_combines = passed_combines;
        summary.failed_combines = failed_combines;
        let total_combines = passed_combines + failed_combines;
        summary.combine_pass_rate = if total_combines > 0 {
            passed_combines as f64 / total_combines as f64 * 100.0
        } else {
            0.0
        };
        summary
    }

    fn in_session(&self, local: &DateTime<Tz>) -> bool {
        let time = local.time();
        let weekday = local.weekday();
        if matches!(weekday, Weekday::Sat | Weekday::Sun) {
            return false;
        }
        match (self.session_start, self.session_end) {
            (Some(start), Some(end)) => time >= start && time < end,
            (Some(start), None) => time >= start,
            (None, Some(end)) => time < end,
            (None, None) => true,
        }
    }

    fn roll_day(&mut self, local: &DateTime<Tz>) {
        let date = local.date_naive();
        match self.current_day {
            None => {
                self.current_day = Some(date);
                self.daily_start_equity = self.prev_equity;
            }
            Some(d) if d != date => {
                self.current_day = Some(date);
                self.daily_start_equity = self.prev_equity;
            }
            _ => {}
        }
    }

    fn current_equity(&self) -> f64 {
        *self.equity.last().unwrap_or(&self.config.starting_cash)
    }

    fn mark_to_market(&mut self, close: f64) {
        let unrealized = if self.position_qty != 0 {
            self.position_direction as f64
                * self.position_qty as f64
                * (close - self.entry_price)
                * self.config.point_value
        } else {
            0.0
        };
        self.equity.push(self.cash + unrealized);
        self.timestamps.push(0); // timestamp filled in by record_equity_ts
    }

    fn record_equity_ts(&mut self, ts: i64) {
        if let Some(last) = self.timestamps.last_mut() {
            *last = ts;
        }
    }

    fn execute_signal(&mut self, signal: Signal, bar: &Bar) {
        let (fill_price, filled) = self.match_signal(&signal, bar);
        if !filled {
            return;
        }

        // Close existing position if signal flips or flattens.
        if self.position_qty != 0
            && (signal.direction == 0 || signal.direction == -self.position_direction)
        {
            self.record_trade(fill_price, bar.timestamp_ns);
        }

        // Open new position if flat and signal is directional.
        if self.position_qty == 0 && (signal.direction == 1 || signal.direction == -1) {
            let qty = self.compute_qty(fill_price, signal.stop_loss);
            self.position_direction = signal.direction;
            self.position_qty = qty;
            self.entry_price = fill_price;
            self.entry_ts = bar.timestamp_ns;
            self.cash -= 2.0 * self.config.commission_per_contract * qty as f64;
        }
    }

    fn match_signal(&self, signal: &Signal, bar: &Bar) -> (f64, bool) {
        if signal.entry_price == 0.0 {
            // Market order at next bar open.
            let price = if signal.direction >= 0 {
                bar.open + self.config.slippage_ticks
            } else {
                bar.open - self.config.slippage_ticks
            };
            return (price, true);
        }

        // Limit order: fill when touched inside the next bar.
        // For a buy limit, fill at the limit price or better (lower). If the
        // bar opens below the limit, the fill is at the open. Otherwise the
        // bar must trade down through the limit, filling at the limit.
        match signal.direction {
            1 if bar.low <= signal.entry_price => {
                let price = bar.open.min(signal.entry_price);
                (price, true)
            }
            -1 if bar.high >= signal.entry_price => {
                let price = bar.open.max(signal.entry_price);
                (price, true)
            }
            _ => (0.0, false),
        }
    }

    fn compute_qty(&self, fill_price: f64, stop_loss: f64) -> i32 {
        if !self.config.position_sizing || fill_price <= 0.0 {
            return 1;
        }
        let equity = self.current_equity();
        let risk_per_unit = if stop_loss > 0.0 {
            (fill_price - stop_loss).abs() * self.config.point_value
        } else {
            fill_price * self.config.point_value
        };
        if risk_per_unit <= 0.0 {
            return 1;
        }
        let qty = (0.01 * equity / risk_per_unit).floor() as i32;
        qty.max(1)
    }

    fn record_trade(&mut self, exit_price: f64, exit_ts: i64) {
        let gross_pnl = self.position_direction as f64
            * self.position_qty as f64
            * (exit_price - self.entry_price)
            * self.config.point_value;
        let commission = 2.0 * self.config.commission_per_contract * self.position_qty as f64;
        let pnl = gross_pnl - commission;
        self.trades.push(Trade {
            entry_ts: self.entry_ts,
            exit_ts,
            entry_price: self.entry_price,
            exit_price,
            direction: self.position_direction,
            qty: self.position_qty,
            pnl,
        });
        self.cash += pnl;
        self.position_qty = 0;
        self.position_direction = 0;
        self.entry_price = 0.0;
    }

    fn flatten_at(&mut self, price: f64, ts: i64) {
        if self.position_qty != 0 {
            self.record_trade(price, ts);
        }
    }

    fn check_risk(&mut self, close: f64, ts: i64) -> bool {
        let equity = *self.equity.last().unwrap_or(&self.config.starting_cash);
        if equity > self.high_water_mark {
            self.high_water_mark = equity;
        }

        let daily_breached = self.config.daily_loss_limit > 0.0
            && equity < self.daily_start_equity - self.config.daily_loss_limit;
        let trailing_breached = self.config.trailing_drawdown > 0.0
            && equity < self.high_water_mark - self.config.trailing_drawdown;

        if !daily_breached && !trailing_breached {
            return false;
        }

        // Risk failure.
        self.failures += 1;
        if daily_breached {
            self.daily_limit_hits += 1;
        }
        if trailing_breached {
            self.trailing_limit_hits += 1;
        }
        self.flatten_at(close, ts);

        if self.config.reset_on_failure {
            self.cash = self.config.starting_cash;
            self.position_direction = 0;
            self.position_qty = 0;
            self.entry_price = 0.0;
            self.pending_signal = None;
            self.high_water_mark = self.config.starting_cash;
            self.daily_start_equity = self.config.starting_cash;
            // Mark to the reset balance so the equity curve reflects the reset.
            self.equity.push(self.config.starting_cash);
            self.timestamps.push(ts);
            true // caller continues after reset
        } else {
            true // caller halts
        }
    }

    fn build_summary(&self) -> Summary {
        let mut summary = Summary::new(
            self.config.starting_cash,
            self.tz,
            &self.timestamps,
            &self.equity,
            &self.trades,
            self.failures,
            &[],
            &[],
        );
        summary.final_equity = *self.equity.last().unwrap_or(&self.config.starting_cash);
        summary.total_pnl = summary.final_equity - self.config.starting_cash;
        summary.executed_trades = self.trades.len();
        summary.daily_limit_hits = self.daily_limit_hits;
        summary.trailing_limit_hits = self.trailing_limit_hits;
        summary.account_failed = self.failures > 0 && !self.config.reset_on_failure;
        summary.account_blowups = if summary.account_failed { 1 } else { self.failures };
        summary.profit_target_reached = self.profit_target_reached;
        summary
    }

    pub fn equity_curve(&self) -> &[f64] {
        &self.equity
    }

    pub fn trades(&self) -> &[Trade] {
        &self.trades
    }
}

fn to_local(timestamp_ns: i64, tz: Tz) -> DateTime<Tz> {
    let secs = timestamp_ns / 1_000_000_000;
    let nanos = (timestamp_ns % 1_000_000_000) as u32;
    DateTime::from_timestamp(secs, nanos)
        .unwrap_or_else(|| DateTime::UNIX_EPOCH)
        .with_timezone(&tz)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FlipFlop;
    impl Strategy for FlipFlop {
        fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
            if bar.close > bar.open {
                Some(Signal {
                    direction: 1,
                    entry_price: 0.0,
                    stop_loss: 0.0,
                    take_profit: 0.0,
                })
            } else {
                Some(Signal {
                    direction: -1,
                    entry_price: 0.0,
                    stop_loss: 0.0,
                    take_profit: 0.0,
                })
            }
        }
    }

    #[test]
    fn test_no_lookahead_execution() {
        // Three bars: 100->110->120. Strategy sees bar 0 (close 110) and goes long.
        // Execution must happen at bar 1 open (120), not at bar 0 close.
        let bars = vec![
            Bar {
                timestamp_ns: 1,
                open: 100.0,
                high: 105.0,
                low: 95.0,
                close: 110.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 2,
                open: 120.0,
                high: 125.0,
                low: 115.0,
                close: 120.0,
                volume: 1,
            },
        ];
        let mut engine = Engine::new(EngineConfig::default());
        let _summary = engine.run(&bars, &mut FlipFlop);
        assert_eq!(engine.trades.len(), 1);
        let trade = &engine.trades[0];
        assert_eq!(trade.entry_price, 120.0); // next-bar open, not 110
        assert_eq!(trade.exit_price, 120.0); // closed at final bar close
    }

    #[test]
    fn test_limit_order_fill_when_touched() {
        // Bar 1 triggers a buy limit at 99. Next bar opens at 100, low 98 -> fill at 99.
        struct BuyLimit;
        impl Strategy for BuyLimit {
            fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
                if bar.timestamp_ns == 1 {
                    Some(Signal {
                        direction: 1,
                        entry_price: 99.0,
                        stop_loss: 0.0,
                        take_profit: 0.0,
                    })
                } else {
                    None
                }
            }
        }

        let bars = vec![
            Bar {
                timestamp_ns: 1,
                open: 100.0,
                high: 101.0,
                low: 99.5,
                close: 101.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 2,
                open: 100.0,
                high: 100.0,
                low: 98.0,
                close: 99.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 3,
                open: 99.0,
                high: 99.0,
                low: 99.0,
                close: 99.0,
                volume: 1,
            },
        ];

        let mut engine = Engine::new(EngineConfig::default());
        let _ = engine.run(&bars, &mut BuyLimit);
        assert_eq!(engine.trades.len(), 1);
        assert_eq!(engine.trades[0].entry_price, 99.0);
    }

    #[test]
    fn test_limit_order_no_fill_when_not_touched() {
        struct BuyLimit;
        impl Strategy for BuyLimit {
            fn on_bar(&mut self, _bar: &Bar) -> Option<Signal> {
                Some(Signal {
                    direction: 1,
                    entry_price: 90.0,
                    stop_loss: 0.0,
                    take_profit: 0.0,
                })
            }
        }

        let bars = vec![
            Bar {
                timestamp_ns: 1,
                open: 100.0,
                high: 101.0,
                low: 95.0,
                close: 100.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 2,
                open: 100.0,
                high: 101.0,
                low: 95.0,
                close: 100.0,
                volume: 1,
            },
        ];

        let mut engine = Engine::new(EngineConfig::default());
        let _ = engine.run(&bars, &mut BuyLimit);
        assert!(engine.trades.is_empty());
    }

    #[test]
    fn test_position_sizing_one_percent() {
        // Price 100, stop 99 -> risk 1 per unit. Equity 10k -> 1% = 100 -> qty 100.
        struct LongWithStop;
        impl Strategy for LongWithStop {
            fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
                if bar.timestamp_ns == 1 {
                    Some(Signal {
                        direction: 1,
                        entry_price: 0.0,
                        stop_loss: 99.0,
                        take_profit: 0.0,
                    })
                } else {
                    None
                }
            }
        }

        let bars = vec![
            Bar {
                timestamp_ns: 1,
                open: 100.0,
                high: 101.0,
                low: 99.0,
                close: 100.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 2,
                open: 100.0,
                high: 100.0,
                low: 100.0,
                close: 100.0,
                volume: 1,
            },
        ];

        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.position_sizing = true;
        let mut engine = Engine::new(config);
        let _ = engine.run(&bars, &mut LongWithStop);
        assert_eq!(engine.trades.len(), 1);
        assert_eq!(engine.trades[0].qty, 100);
    }

    #[test]
    fn test_daily_loss_limit_triggers_reset() {
        // Equity drops from 10k to 9k in one bar; daily loss limit 500 -> reset.
        struct GoLong;
        impl Strategy for GoLong {
            fn on_bar(&mut self, bar: &Bar) -> Option<Signal> {
                if bar.timestamp_ns == 1 {
                    Some(Signal {
                        direction: 1,
                        entry_price: 0.0,
                        stop_loss: 0.0,
                        take_profit: 0.0,
                    })
                } else {
                    None
                }
            }
        }

        let bars = vec![
            Bar {
                timestamp_ns: 1,
                open: 100.0,
                high: 100.0,
                low: 100.0,
                close: 100.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 2,
                open: 100.0,
                high: 100.0,
                low: 40.0,
                close: 40.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 3,
                open: 100.0,
                high: 100.0,
                low: 100.0,
                close: 100.0,
                volume: 1,
            },
        ];

        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.daily_loss_limit = 500.0;
        config.position_sizing = false;
        config.point_value = 10.0;
        let mut engine = Engine::new(config);
        let summary = engine.run(&bars, &mut GoLong);
        assert!(summary.failures >= 1, "expected daily loss limit breach, got failures={}", summary.failures);
        // After reset the position is flat, so the final winning bar should not recover the loss.
        assert_eq!(engine.position_qty, 0);
    }

    #[test]
    fn test_session_filter_skips_night_bars() {
        // With an empty session window, no trades should occur.
        struct AlwaysLong;
        impl Strategy for AlwaysLong {
            fn on_bar(&mut self, _bar: &Bar) -> Option<Signal> {
                Some(Signal {
                    direction: 1,
                    entry_price: 0.0,
                    stop_loss: 0.0,
                    take_profit: 0.0,
                })
            }
        }

        let bars = vec![
            Bar {
                timestamp_ns: 0,
                open: 100.0,
                high: 100.0,
                low: 100.0,
                close: 100.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 10_000_000_000,
                open: 100.0,
                high: 100.0,
                low: 100.0,
                close: 100.0,
                volume: 1,
            },
            Bar {
                timestamp_ns: 20_000_000_000,
                open: 100.0,
                high: 100.0,
                low: 100.0,
                close: 100.0,
                volume: 1,
            },
        ];

        let mut config = EngineConfig::default();
        config.timezone = "UTC".to_string();
        config.session_start = Some("00:00".to_string());
        config.session_end = Some("00:00".to_string());
        let mut engine = Engine::new(config);
        let _ = engine.run(&bars, &mut AlwaysLong);
        assert!(engine.trades.is_empty());
    }

    #[test]
    fn test_run_signals_basic() {
        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.point_value = 10.0;
        let mut engine = Engine::new(config);

        let mut trades = vec![PrefilledTrade {
            entry_ts: 1,
            exit_ts: 2,
            direction: 1,
            entry_price: 100.0,
            exit_price: 110.0,
            pnl_points: 10.0,
        }];
        let ts = TopstepConfig::default();
        let summary = engine.run_signals(&mut trades, &ts);
        assert_eq!(summary.n_trades, 1);
        assert!((summary.total_pnl - 100.0).abs() < 1e-9);
    }

    #[test]
    fn test_run_signals_daily_limit_skips_rest_of_day() {
        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.point_value = 1.0;
        let mut engine = Engine::new(config);

        // First trade would breach the $500 daily drawdown limit and is skipped.
        // Second trade is on the same calendar day, so it is also skipped.
        // Third trade is on the next day and executes normally.
        let day_ns = 86_400_000_000_000i64;
        let mut trades = vec![
            PrefilledTrade {
                entry_ts: 1,
                exit_ts: 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 90.0,
                pnl_points: -1000.0,
            },
            PrefilledTrade {
                entry_ts: 3,
                exit_ts: 4,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 10.0,
            },
            PrefilledTrade {
                entry_ts: day_ns + 1,
                exit_ts: day_ns + 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 10.0,
            },
        ];
        let ts = TopstepConfig {
            enabled: true,
            daily_drawdown_limit: 500.0,
            trailing_drawdown_limit: 0.0,
            profit_target: 0.0,
            reset_on_failure: true,
        };
        let summary = engine.run_signals(&mut trades, &ts);
        assert!(summary.daily_limit_hits >= 1);
        assert!(summary.failures >= 1);
        assert_eq!(summary.n_trades, 1);
        assert!((summary.total_pnl - 10.0).abs() < 1e-9);
    }

    #[test]
    fn test_run_signals_profit_target_does_not_stop_trading() {
        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.point_value = 1.0;
        let mut engine = Engine::new(config);

        let mut trades = vec![
            PrefilledTrade {
                entry_ts: 1,
                exit_ts: 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 4000.0,
            },
            PrefilledTrade {
                entry_ts: 3,
                exit_ts: 4,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 10.0,
            },
        ];
        let ts = TopstepConfig {
            enabled: true,
            daily_drawdown_limit: 0.0,
            trailing_drawdown_limit: 0.0,
            profit_target: 3_000.0,
            reset_on_failure: true,
        };
        let summary = engine.run_signals(&mut trades, &ts);
        assert!(summary.profit_target_reached);
        assert_eq!(summary.n_trades, 2);
        assert!((summary.total_pnl - 4010.0).abs() < 1e-9);
    }

    #[test]
    fn test_run_signals_tracks_passed_combine_stats() {
        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.point_value = 1.0;
        let mut engine = Engine::new(config);

        let day_ns = 86_400_000_000_000i64;
        let mut trades = vec![
            // Combine 1: trade on day 1, then pass on day 2 (2 distinct days, 2 trades).
            PrefilledTrade {
                entry_ts: 1,
                exit_ts: 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 1000.0,
            },
            PrefilledTrade {
                entry_ts: day_ns + 1,
                exit_ts: day_ns + 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 2500.0,
            },
            // Combine 2: pass in a single trade on day 3.
            PrefilledTrade {
                entry_ts: 2 * day_ns + 1,
                exit_ts: 2 * day_ns + 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 3500.0,
            },
        ];
        let ts = TopstepConfig {
            enabled: true,
            daily_drawdown_limit: 0.0,
            trailing_drawdown_limit: 0.0,
            profit_target: 3_000.0,
            reset_on_failure: true,
        };
        let summary = engine.run_signals(&mut trades, &ts);
        assert_eq!(summary.passed_combines, 2);
        assert_eq!(summary.failed_combines, 0);
        assert!((summary.combine_pass_rate - 100.0).abs() < 1e-9);
        assert_eq!(summary.avg_days_to_pass, 1.5);
        assert_eq!(summary.min_days_to_pass, 1);
        assert_eq!(summary.max_days_to_pass, 2);
        assert_eq!(summary.avg_trades_to_pass, 1.5);
    }

    #[test]
    fn test_run_signals_failed_combine_counts_trailing_breach() {
        let mut config = EngineConfig::default();
        config.starting_cash = 10_000.0;
        config.point_value = 1.0;
        let mut engine = Engine::new(config);

        let mut trades = vec![
            PrefilledTrade {
                entry_ts: 1,
                exit_ts: 2,
                direction: 1,
                entry_price: 100.0,
                exit_price: 110.0,
                pnl_points: 3500.0,
            },
            // Trailing drawdown breach after the profit target reset.
            PrefilledTrade {
                entry_ts: 3,
                exit_ts: 4,
                direction: 1,
                entry_price: 100.0,
                exit_price: 90.0,
                pnl_points: -2500.0,
            },
        ];
        let ts = TopstepConfig {
            enabled: true,
            daily_drawdown_limit: 0.0,
            trailing_drawdown_limit: 2_000.0,
            profit_target: 3_000.0,
            reset_on_failure: true,
        };
        let summary = engine.run_signals(&mut trades, &ts);
        assert_eq!(summary.passed_combines, 1);
        assert_eq!(summary.failed_combines, 1);
        assert!((summary.combine_pass_rate - 50.0).abs() < 1e-9);
        assert_eq!(summary.days_to_pass_p50, 1.0);
        assert_eq!(summary.median_trades_to_pass, 1.0);
    }
}
