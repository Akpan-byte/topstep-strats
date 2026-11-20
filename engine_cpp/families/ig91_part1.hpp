#pragma once
#include "ig91_common.hpp"

namespace s546 {
namespace ig91 {

enum P1Kind : int {
    P1_001_EMA9_RETEST = 0,
    P1_001_ORBV5_CONF,
    P1_002_ASIA_IFVG,
    P1_003_BREAKER_CHECKLIST,
    P1_004_ENGULFING_50PCT,
    P1_005_IFVG_AGGR_STOP,
    P1_006_BIG_LITTLE_BIG,
    P1_007_BPR_RETRACE,
    P1_008_CISD_CONF,
    P1_009_REAL_LIQ_SWEEP,
    P1_010_FOUR_CONF_9PT,
    P1_011_SWEEP_15M_REJ,
    P1_012_CONSOL_BOS,
    P1_013_FVG_REV_SCALP,
    P1_014_TRENDS_VP_AMD,
    P1_015_AMD_POC_PULLBACK,
    P1_016_ORB_REJ_RETEST,
    P1_017_TDO_SD_RANGES,
    P1_018_LONDON_SWEEP_50,
    P1_019_OTE_FIB_705,
    P1_020_UNICORN_BREAKER,
    P1_021_SWEEP_IFVG_TL,
    P1_022_DAILY_DRIFT_LONG,
    P1_023_4H_PD_15M_REJ50,
    P1_024_30M_VP_POC,
    P1_025_SESSION_SWITCH,
    P1_026_RSI_DIV_5M,
    P1_027_SESS_LIQ_OTE_5R,
    P1_028_SD_INDECISION,
    P1_029_915_FVG_V_REC,
    P1_030_NY_PM_FIRST_FVG
};

struct P1State {
    int kind = 0;
    u11::InstrumentSpec inst{};
    std::vector<double> params;

    BarAggregator tf5{}, tf15{}, tf30{}, tf60{}, tf240{};
    FvgTracker fvg1{}, fvg5{}, fvg15{}, fvg240{};
    SwingTracker sw1{}, sw5{}, sw15{};
    VolumeProfileTracker vp{};
    SessionLevelsTracker sess{};
    IndicatorPack ind{};

    // State variables for specific reels
    int32_t cur_day = -1;
    bool ema9_touched = false;
    double r1_body_hi = 0.0, r1_body_lo = 0.0;
    bool r1_range_set = false;
    int r1_dir_bias = 0;

    // 006 / 025 / 028 Supply/Demand Zone
    bool has_sd_zone = false;
    int8_t sd_dir = 0;
    double sd_hi = 0.0, sd_lo = 0.0;
    int sd_age = 0;

    // 016 ORB rejection counter
    int orb_rej_hi = 0, orb_rej_lo = 0;

    // 026 RSI divergence tracking
    double prev_sw_l = 0.0, prev_sw_l_rsi = 50.0;
    double prev_sw_h = 0.0, prev_sw_h_rsi = 50.0;

    // 030 First presented FVG in PM ORB
    bool pm_fvg_captured = false;
    FvgZone pm_fvg{};
};

template <int K>
inline void p1_init(P1State& st, const std::vector<double>& p, const u11::InstrumentSpec* inst) {
    st = P1State{};
    st.kind = K;
    st.inst = inst ? *inst : u11::get_instrument_spec("NQ");
    st.params = p;
    st.tf5.reset(5, 32);
    st.tf15.reset(15, 32);
    st.tf30.reset(30, 32);
    st.tf60.reset(60, 24);
    st.tf240.reset(240, 16);
    st.fvg1.reset(12);
    st.fvg5.reset(12);
    st.fvg15.reset(12);
    st.fvg240.reset(8);
    st.sw1.reset(3);
    st.sw5.reset(2);
    st.sw15.reset(2);
    st.vp.reset(std::max(1.0, 5.0 * sym_pt_scale(st.inst)));
    st.ind.reset();
}

inline u11::PluginSignal p1_on_bar(const u11::CausalBarView& v, void* state_ptr) {
    auto* st = static_cast<P1State*>(state_ptr);
    const auto& p = st->params;
    const auto& inst = st->inst;

    // Update all causal trackers
    st->tf5.update(v);
    st->tf15.update(v);
    st->tf30.update(v);
    st->tf60.update(v);
    st->tf240.update(v);
    st->sess.update(v);
    st->ind.update(v, inst);

    double o = static_cast<double>(v[0].o);
    double h = static_cast<double>(v[0].h);
    double l = static_cast<double>(v[0].l);
    double c = static_cast<double>(v[0].c);
    double vol = static_cast<double>(v[0].v);
    double atr = std::max(inst.tick_size * 2.0, v.atr14());
    int16_t em = v.et_mins();
    int32_t ymd = v.day_yyyymmdd();

    if (ymd != st->cur_day) {
        st->cur_day = ymd;
        st->ema9_touched = false;
        st->r1_range_set = false;
        st->r1_dir_bias = 0;
        st->orb_rej_hi = 0;
        st->orb_rej_lo = 0;
        st->pm_fvg_captured = false;
        if (st->kind == P1_015_AMD_POC_PULLBACK || st->kind == P1_014_TRENDS_VP_AMD) {
            st->vp.reset(std::max(1.0, 4.0 * sym_pt_scale(inst)));
        }
    }

    st->vp.add_bar(h, l, c, vol);
    if (v.bar_index() % 5 == 0) st->vp.compute();

    AggBar b1{o, h, l, c, vol, static_cast<int64_t>(v.bar_index()), true};
    AggBar b1_1{static_cast<double>(v[-1].o), static_cast<double>(v[-1].h), static_cast<double>(v[-1].l), static_cast<double>(v[-1].c), static_cast<double>(v[-1].v), 0, v.bar_index() >= 1};
    AggBar b1_2{static_cast<double>(v[-2].o), static_cast<double>(v[-2].h), static_cast<double>(v[-2].l), static_cast<double>(v[-2].c), static_cast<double>(v[-2].v), 0, v.bar_index() >= 2};
    st->fvg1.on_closed_bar(b1, b1_1, b1_2, inst.tick_size);
    st->sw1.update(b1);

    if (st->tf5.just_closed) {
        st->fvg5.on_closed_bar(st->tf5.bar(0), st->tf5.bar(1), st->tf5.bar(2), inst.tick_size);
        st->sw5.update(st->tf5.bar(0));
    }
    if (st->tf15.just_closed) {
        st->fvg15.on_closed_bar(st->tf15.bar(0), st->tf15.bar(1), st->tf15.bar(2), inst.tick_size);
        st->sw15.update(st->tf15.bar(0));
    }
    if (st->tf240.just_closed) {
        st->fvg240.on_closed_bar(st->tf240.bar(0), st->tf240.bar(1), st->tf240.bar(2), inst.tick_size);
    }

    if (st->has_sd_zone) {
        st->sd_age++;
        if (st->sd_age > 180) st->has_sd_zone = false;
    }

    if (v.bar_index() < 30 || v.in_position) return {};

    // Dispatch per reel
    switch (st->kind) {
    case P1_001_EMA9_RETEST: {
        // 5m 9-EMA retest range breakout
        if (em < 510 || em >= 720) return {};
        if (!st->tf5.just_closed) return {};
        const auto& b = st->tf5.bar(0);
        double e9 = v.ema9(-1);
        if (!st->ema9_touched) {
            if (b.l <= e9 && b.h >= e9) {
                st->ema9_touched = true;
                st->r1_range_set = false;
            }
            return {};
        }
        if (!st->r1_range_set) {
            // Pullback/retest candle after touching 9 EMA sets body high/low range
            st->r1_body_hi = std::max(b.o, b.c);
            st->r1_body_lo = std::min(b.o, b.c);
            if (st->r1_body_hi > st->r1_body_lo) st->r1_range_set = true;
            return {};
        }
        double sl_mult = p_at(p, 0, 1.5);
        double rr = p_at(p, 1, 2.0);
        double sl = std::max(st->r1_body_hi - st->r1_body_lo, atr * sl_mult);
        if (b.c > st->r1_body_hi) {
            st->ema9_touched = false;
            st->r1_range_set = false;
            return make_sig(1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 60, 2, inst);
        }
        if (b.c < st->r1_body_lo) {
            st->ema9_touched = false;
            st->r1_range_set = false;
            return make_sig(-1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 60, 2, inst);
        }
        return {};
    }

    case P1_001_ORBV5_CONF: {
        // ORBv5 (15m ORB) + 9-EMA retest range confluence
        if (st->tf5.just_closed) {
            const auto& b = st->tf5.bar(0);
            double e9 = v.ema9(-1);
            if (b.l <= e9 && b.h >= e9) { st->ema9_touched = true; st->r1_range_set = false; }
            else if (st->ema9_touched && !st->r1_range_set) {
                st->r1_body_hi = std::max(b.o, b.c);
                st->r1_body_lo = std::min(b.o, b.c);
                st->r1_range_set = true;
            } else if (st->r1_range_set) {
                if (b.c > st->r1_body_hi) st->r1_dir_bias = 1;
                else if (b.c < st->r1_body_lo) st->r1_dir_bias = -1;
            }
        }
        if (!v.or_formed(15) || em < 585 || em >= 840) return {};
        if (v.sess_entries >= static_cast<int>(p_at(p, 2, 2.0))) return {};
        double orh = v.or_high(15), orl = v.or_low(15);
        double sl_mult = p_at(p, 0, 0.5);
        double rr = p_at(p, 1, 2.5);
        double sl = std::max(atr * 1.2, (orh - orl) * sl_mult);
        if (c > orh && c > v.ema9() && (st->r1_dir_bias == 1 || !st->r1_range_set)) {
            return make_sig(1, 0, 0.0, sl, sl * rr, sl * 0.7, sl * 0.35, sl * 0.6, inst.tick_size, 90, 5, inst);
        }
        if (c < orl && c < v.ema9() && (st->r1_dir_bias == -1 || !st->r1_range_set)) {
            return make_sig(-1, 0, 0.0, sl, sl * rr, sl * 0.7, sl * 0.35, sl * 0.6, inst.tick_size, 90, 5, inst);
        }
        return {};
    }

    case P1_002_ASIA_IFVG: {
        // Asia session (20:00-23:59 ET) long reversal with 1m IFVG + discount below 15m mid
        if (em < 1200 || em > 1400) return {};
        FvgZone z;
        double sl_mult = p_at(p, 0, 1.5);
        double rr = p_at(p, 1, 2.5);
        if (st->fvg1.find_tap(l, h, 1, true, 15, z) && c > o && c < v.ema50()) {
            double sl = std::max(atr * sl_mult, c - z.lo + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, sl * 0.8, inst.tick_size, 45, 3, inst);
        }
        return {};
    }

    case P1_003_BREAKER_CHECKLIST: {
        // 5m Breaker Block (broken swing retest) + EMA9/20 trend + non-choppy ATR
        if (!st->tf5.just_closed || !st->sw5.has_sh || !st->sw5.has_sl) return {};
        double e9 = v.ema9(-1), e20 = v.ema20(-1);
        double sl_pts = atr * p_at(p, 0, 1.5);
        double rr = p_at(p, 1, 2.0);
        if (st->sw5.trend_dir == 1 && e9 > e20 && l <= st->sw5.prev_sh && c > st->sw5.prev_sh) {
            return make_sig(1, 0, 0.0, sl_pts, sl_pts * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        if (st->sw5.trend_dir == -1 && e9 < e20 && h >= st->sw5.prev_sl && c < st->sw5.prev_sl) {
            return make_sig(-1, 0, 0.0, sl_pts, sl_pts * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        return {};
    }

    case P1_004_ENGULFING_50PCT: {
        // 5m engulfing candle -> Limit entry at 50% of engulfing candle
        if (!st->tf5.just_closed || st->tf5.size() < 2) return {};
        const auto& b0 = st->tf5.bar(0);
        const auto& b1 = st->tf5.bar(1);
        double rr = p_at(p, 0, 2.0);
        if (b1.c < b1.o && b0.c > b0.o && b0.c > b1.o && b0.o <= b1.c) {
            double mid = 0.5 * (b0.h + b0.l);
            double sl = std::max(atr, mid - b0.l + inst.tick_size);
            return make_sig(1, 2, mid, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 2, inst);
        }
        if (b1.c > b1.o && b0.c < b0.o && b0.c < b1.o && b0.o >= b1.c) {
            double mid = 0.5 * (b0.h + b0.l);
            double sl = std::max(atr, b0.h - mid + inst.tick_size);
            return make_sig(-1, 2, mid, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 2, inst);
        }
        return {};
    }

    case P1_005_IFVG_AGGR_STOP: {
        // BOS + FVG retest with tight stop at FVG edge
        FvgZone z;
        double rr = p_at(p, 0, 2.5);
        if (st->sw1.trend_dir == 1 && st->fvg1.find_tap(l, h, 1, false, 15, z) && c > o) {
            double sl = std::max(atr * 0.8, c - z.lo + inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 40, 3, inst);
        }
        if (st->sw1.trend_dir == -1 && st->fvg1.find_tap(l, h, -1, false, 15, z) && c < o) {
            double sl = std::max(atr * 0.8, z.hi - c + inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 40, 3, inst);
        }
        return {};
    }

    case P1_006_BIG_LITTLE_BIG: {
        // 15m Big impulse -> Small indecision -> Big impulse; enter on tap of small candle
        if (st->tf15.just_closed && st->tf15.size() >= 3) {
            const auto& b0 = st->tf15.bar(0);
            const auto& b1 = st->tf15.bar(1);
            const auto& b2 = st->tf15.bar(2);
            double body0 = std::abs(b0.c - b0.o), body1 = std::abs(b1.c - b1.o), body2 = std::abs(b2.c - b2.o);
            if (body1 < 0.4 * body0 && body1 < 0.4 * body2) {
                if (b0.c > b0.o && b2.c > b2.o) {
                    st->has_sd_zone = true; st->sd_dir = 1; st->sd_hi = b1.h; st->sd_lo = b1.l; st->sd_age = 0;
                } else if (b0.c < b0.o && b2.c < b2.o) {
                    st->has_sd_zone = true; st->sd_dir = -1; st->sd_hi = b1.h; st->sd_lo = b1.l; st->sd_age = 0;
                }
            }
        }
        if (st->has_sd_zone) {
            double rr = p_at(p, 0, 2.0);
            if (st->sd_dir == 1 && l <= st->sd_hi && c >= st->sd_lo && c > o) {
                st->has_sd_zone = false;
                double sl = std::max(atr, c - st->sd_lo + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
            }
            if (st->sd_dir == -1 && h >= st->sd_lo && c <= st->sd_hi && c < o) {
                st->has_sd_zone = false;
                double sl = std::max(atr, st->sd_hi - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
            }
        }
        return {};
    }

    case P1_007_BPR_RETRACE: {
        // Balanced Price Range (overlapping bull+bear FVG) retrace
        if (st->fvg1.has_bpr) {
            double rr = p_at(p, 0, 2.5);
            if (st->fvg1.bpr_dir == 1 && l <= st->fvg1.bpr_hi && c >= st->fvg1.bpr_lo && c > o) {
                st->fvg1.has_bpr = false;
                double sl = std::max(atr, c - st->fvg1.bpr_lo + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
            }
            if (st->fvg1.bpr_dir == -1 && h >= st->fvg1.bpr_lo && c <= st->fvg1.bpr_hi && c < o) {
                st->fvg1.has_bpr = false;
                double sl = std::max(atr, st->fvg1.bpr_hi - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
            }
        }
        return {};
    }

    case P1_008_CISD_CONF: {
        // Sweep + IFVG + CISD (close through prior opposing candle's open)
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->sw1.sweep_lo_now || (st->fvg1.find_tap(l, h, 1, true, 12, z) && c > static_cast<double>(v[-1].o) && static_cast<double>(v[-1].c) < static_cast<double>(v[-1].o))) {
            double sl = atr * p_at(p, 1, 1.5);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        if (st->sw1.sweep_hi_now || (st->fvg1.find_tap(l, h, -1, true, 12, z) && c < static_cast<double>(v[-1].o) && static_cast<double>(v[-1].c) > static_cast<double>(v[-1].o))) {
            double sl = atr * p_at(p, 1, 1.5);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        return {};
    }

    case P1_009_REAL_LIQ_SWEEP: {
        // Real liquidity sweep: sweep of swing H/L with high relative volume + close back inside
        double rvol_min = p_at(p, 0, 1.3);
        double rr = p_at(p, 1, 2.0);
        if (v.rel_vol() >= rvol_min) {
            if (st->sw1.sweep_lo_now && c > o) {
                double sl = std::max(atr, c - l + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
            }
            if (st->sw1.sweep_hi_now && c < o) {
                double sl = std::max(atr, h - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
            }
        }
        return {};
    }

    case P1_010_FOUR_CONF_9PT: {
        // 5m sweep + 1m IFVG >= 9 pts (scaled)
        double min_gap = p_at(p, 0, 9.0) * sym_pt_scale(inst);
        double rr = p_at(p, 1, 2.0);
        FvgZone z;
        if (st->sw5.has_sl && st->fvg1.find_tap(l, h, 1, true, 20, z) && (z.hi - z.lo) >= min_gap) {
            double sl = std::max(atr * 1.2, c - z.lo + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (st->sw5.has_sh && st->fvg1.find_tap(l, h, -1, true, 20, z) && (z.hi - z.lo) >= min_gap) {
            double sl = std::max(atr * 1.2, z.hi - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P1_011_SWEEP_15M_REJ: {
        // Sweep of session H/L + 15m rejection wick
        if (!st->tf15.just_closed) return {};
        const auto& b = st->tf15.bar(0);
        double rng = std::max(inst.tick_size, b.h - b.l);
        double rr = p_at(p, 0, 2.0);
        if (st->sess.asia_ok && b.l < st->sess.asia_l && b.c > st->sess.asia_l && (std::min(b.o, b.c) - b.l) / rng >= 0.35) {
            double sl = std::max(atr * 1.5, b.c - b.l + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (st->sess.asia_ok && b.h > st->sess.asia_h && b.c < st->sess.asia_h && (b.h - std::max(b.o, b.c)) / rng >= 0.35) {
            double sl = std::max(atr * 1.5, b.h - b.c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P1_012_CONSOL_BOS: {
        // Consolidation breakout + strong displacement candle close
        double body = std::abs(c - o);
        double rng = std::max(inst.tick_size, h - l);
        double rr = p_at(p, 0, 2.0);
        if (body >= 0.65 * rng && body >= atr * 1.1) {
            if (st->sw1.bos_up_now && c > o) {
                return make_sig(1, 0, 0.0, atr * 1.5, atr * 1.5 * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
            }
            if (st->sw1.bos_dn_now && c < o) {
                return make_sig(-1, 0, 0.0, atr * 1.5, atr * 1.5 * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
            }
        }
        return {};
    }

    case P1_013_FVG_REV_SCALP: {
        // Sweep + FVG tap + CISD candle -> 2R
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->fvg1.find_tap(l, h, 1, false, 10, z) && c > o && c > static_cast<double>(v[-1].h)) {
            double sl = std::max(atr, c - l + inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 30, 2, inst);
        }
        if (st->fvg1.find_tap(l, h, -1, false, 10, z) && c < o && c < static_cast<double>(v[-1].l)) {
            double sl = std::max(atr, h - c + inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 30, 2, inst);
        }
        return {};
    }

    case P1_014_TRENDS_VP_AMD: {
        // Trend (EMA9 > EMA20 > EMA50) + price above/below VP POC
        if (!st->vp.valid) return {};
        double rr = p_at(p, 0, 2.0);
        double sl = atr * p_at(p, 1, 1.5);
        if (v.ema9() > v.ema20() && v.ema20() > v.ema50() && c > st->vp.poc && l <= st->vp.poc + atr * 0.5 && c > o) {
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (v.ema9() < v.ema20() && v.ema20() < v.ema50() && c < st->vp.poc && h >= st->vp.poc - atr * 0.5 && c < o) {
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P1_015_AMD_POC_PULLBACK: {
        // Manipulation sweep + pullback to accumulation POC (RTH gated, max 3/session)
        if (em < 570 || em >= 960 || v.sess_entries >= 3 || !st->vp.valid) return {};
        double rr = p_at(p, 0, 2.5);
        double sl = atr * p_at(p, 1, 1.5);
        if (st->sw1.trend_dir == 1 && l <= st->vp.poc && c > st->vp.poc && c > o) {
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        if (st->sw1.trend_dir == -1 && h >= st->vp.poc && c < st->vp.poc && c < o) {
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        return {};
    }

    case P1_016_ORB_REJ_RETEST: {
        // 15m ORB: 2+ rejections at ORH/ORL -> fade; or momentum break -> trend
        if (!v.or_formed(15) || em < 585 || em >= 840) return {};
        double orh = v.or_high(15), orl = v.or_low(15);
        double sl = std::max(atr * 1.5, 0.5 * (orh - orl));
        double rr = p_at(p, 0, 2.0);
        if (h >= orh && c < orh) st->orb_rej_hi++;
        if (l <= orl && c > orl) st->orb_rej_lo++;
        if (st->orb_rej_hi >= 2 && c < o) {
            st->orb_rej_hi = 0;
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        if (st->orb_rej_lo >= 2 && c > o) {
            st->orb_rej_lo = 0;
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        return {};
    }

    case P1_017_TDO_SD_RANGES: {
        // True Day Open (00:00 ET) + 8:30 AM ET open -> mean reversion to equilibrium
        if (!st->sess.has_tdo || !st->sess.has_830 || em < 515 || em >= 690) return {};
        double eq = 0.5 * (st->sess.tdo_open + st->sess.open_830);
        double dist = std::abs(c - eq);
        if (dist < atr * 1.5) return {};
        double sl = atr * p_at(p, 0, 1.8);
        if (c > st->sess.open_830 && c > st->sess.tdo_open && c < o) {
            return make_sig(-1, 0, 0.0, sl, dist * 0.85, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        if (c < st->sess.tdo_open && st->sess.open_830 > st->sess.tdo_open && c > o) {
            return make_sig(1, 0, 0.0, sl, dist * 0.85, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        return {};
    }

    case P1_018_LONDON_SWEEP_50: {
        // Sweep of London H/L -> 50% retrace of London range
        if (!st->sess.lon_ok || em < 510 || em >= 720) return {};
        double mid = 0.5 * (st->sess.lon_h + st->sess.lon_l);
        double rr = p_at(p, 0, 2.0);
        double sl = atr * p_at(p, 1, 1.5);
        if (l < st->sess.lon_l && c > st->sess.lon_l && c < mid) {
            return make_sig(1, 0, 0.0, sl, std::max(sl * rr, mid - c), 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        if (h > st->sess.lon_h && c < st->sess.lon_h && c > mid) {
            return make_sig(-1, 0, 0.0, sl, std::max(sl * rr, c - mid), 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        return {};
    }

    case P1_019_OTE_FIB_705: {
        // ICT OTE 70.5% Fib pullback after BOS
        if (!st->sw1.has_sh || !st->sw1.has_sl) return {};
        double rng = st->sw1.last_sh - st->sw1.last_sl;
        if (rng < atr * 2.0) return {};
        double rr = p_at(p, 0, 2.0);
        if (st->sw1.trend_dir == 1) {
            double ote = st->sw1.last_sh - 0.705 * rng;
            if (l <= ote && c >= st->sw1.last_sl && c > o) {
                double sl = std::max(atr, c - st->sw1.last_sl + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            }
        } else if (st->sw1.trend_dir == -1) {
            double ote = st->sw1.last_sl + 0.705 * rng;
            if (h >= ote && c <= st->sw1.last_sh && c < o) {
                double sl = std::max(atr, st->sw1.last_sh - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            }
        }
        return {};
    }

    case P1_020_UNICORN_BREAKER: {
        // Breaker Block + FVG overlap (Unicorn)
        FvgZone z;
        double rr = p_at(p, 0, 2.5);
        if (st->sw5.has_sh && st->fvg1.find_tap(l, h, 1, false, 15, z)) {
            if (z.lo <= st->sw5.prev_sh && z.hi >= st->sw5.prev_sh * 0.999) {
                double sl = std::max(atr * 1.2, c - z.lo + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            }
        }
        if (st->sw5.has_sl && st->fvg1.find_tap(l, h, -1, false, 15, z)) {
            if (z.hi >= st->sw5.prev_sl && z.lo <= st->sw5.prev_sl * 1.001) {
                double sl = std::max(atr * 1.2, z.hi - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            }
        }
        return {};
    }

    case P1_021_SWEEP_IFVG_TL: {
        // Sweep + IFVG + 9EMA/20EMA trendline alignment
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->fvg1.find_tap(l, h, 1, true, 15, z) && c > v.ema9() && v.ema9() > v.ema20()) {
            double sl = std::max(atr * 1.2, c - z.lo + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        if (st->fvg1.find_tap(l, h, -1, true, 15, z) && c < v.ema9() && v.ema9() < v.ema20()) {
            double sl = std::max(atr * 1.2, z.hi - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        return {};
    }

    case P1_022_DAILY_DRIFT_LONG: {
        // Long-only intraday drift with ATR trailing ratchet
        if (em < 570 || em >= 900) return {};
        if (v.sess_entries >= 1) return {};
        double sl = atr * p_at(p, 0, 2.5);
        double r_trig = atr * p_at(p, 1, 1.5);
        double r_trail = atr * p_at(p, 2, 1.2);
        if (v.ema20() > v.ema50() && c > v.session_vwap() && c > o) {
            return make_sig(1, 0, 0.0, sl, 0.0, r_trig, r_trail, 0.0, 0.0, 240, 10, inst);
        }
        return {};
    }

    case P1_023_4H_PD_15M_REJ50: {
        // 4H FVG/PD array tap -> 15m rejection wick -> 50% limit entry, tight SL, 6R target
        if (!st->tf15.just_closed) return {};
        const auto& b = st->tf15.bar(0);
        double rng = std::max(inst.tick_size, b.h - b.l);
        double sl_pts = p_at(p, 0, 10.0) * sym_pt_scale(inst);
        double rr = p_at(p, 1, 6.0);
        FvgZone z;
        if (st->fvg240.find_tap(b.l, b.h, 1, false, 16, z) && (std::min(b.o, b.c) - b.l) / rng >= 0.35) {
            double entry = 0.5 * (b.l + std::min(b.o, b.c));
            return make_sig(1, 2, entry, sl_pts, sl_pts * rr, 0.0, 0.0, sl_pts * 1.5, inst.tick_size, 90, 4, inst);
        }
        if (st->fvg240.find_tap(b.l, b.h, -1, false, 16, z) && (b.h - std::max(b.o, b.c)) / rng >= 0.35) {
            double entry = 0.5 * (b.h + std::max(b.o, b.c));
            return make_sig(-1, 2, entry, sl_pts, sl_pts * rr, 0.0, 0.0, sl_pts * 1.5, inst.tick_size, 90, 4, inst);
        }
        return {};
    }

    case P1_024_30M_VP_POC: {
        // 30m accumulation VP breakout -> pullback to POC -> continuation
        if (!st->vp.valid) return {};
        double rr = p_at(p, 0, 2.0);
        double sl = atr * p_at(p, 1, 1.5);
        if (c > st->vp.vah && st->sw15.trend_dir == 1 && l <= st->vp.poc + atr) {
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (c < st->vp.val && st->sw15.trend_dir == -1 && h >= st->vp.poc - atr) {
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P1_025_SESSION_SWITCH: {
        // Session Switch Strategy: during session transitions (02:45-03:30, 08:15-09:45, 13:15-13:45 ET)
        // find last 5m indecision candle before big impulse -> enter limit/retest at zone
        if (st->tf5.just_closed && st->tf5.size() >= 2) {
            const auto& imp = st->tf5.bar(0);
            const auto& ind_c = st->tf5.bar(1);
            double imp_body = std::abs(imp.c - imp.o);
            double ind_body = std::abs(ind_c.c - ind_c.o);
            if (imp_body >= atr * 1.5 && ind_body <= 0.45 * imp_body) {
                st->has_sd_zone = true;
                st->sd_dir = (imp.c > imp.o) ? 1 : -1;
                st->sd_hi = ind_c.h;
                st->sd_lo = ind_c.l;
                st->sd_age = 0;
            }
        }
        bool in_switch = (em >= 165 && em <= 210) || (em >= 495 && em <= 585) || (em >= 795 && em <= 825);
        if (in_switch && st->has_sd_zone) {
            double rr = p_at(p, 0, 2.5);
            double sl_pad = atr * p_at(p, 1, 0.5);
            if (st->sd_dir == 1 && l <= st->sd_hi && c >= st->sd_lo) {
                st->has_sd_zone = false;
                double sl = std::max(atr, c - st->sd_lo + sl_pad);
                return make_sig(1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 60, 3, inst);
            }
            if (st->sd_dir == -1 && h >= st->sd_lo && c <= st->sd_hi) {
                st->has_sd_zone = false;
                double sl = std::max(atr, st->sd_hi - c + sl_pad);
                return make_sig(-1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 60, 3, inst);
            }
        }
        return {};
    }

    case P1_026_RSI_DIV_5M: {
        // 5m RSI(14) divergence, short hold (1-12 bars)
        if (!st->tf5.just_closed) return {};
        const auto& b = st->tf5.bar(0);
        double rsi = st->ind.rsi14;
        int hold = static_cast<int>(p_at(p, 0, 12.0));
        double sl = atr * p_at(p, 1, 2.0);
        u11::PluginSignal sig{};
        if (st->prev_sw_l > 0.0 && b.l < st->prev_sw_l && rsi > st->prev_sw_l_rsi && rsi < 45.0) {
            sig = make_sig(1, 0, 0.0, sl, sl * 1.5, 0.0, 0.0, 0.0, 0.0, hold, 2, inst);
        } else if (st->prev_sw_h > 0.0 && b.h > st->prev_sw_h && rsi < st->prev_sw_h_rsi && rsi > 55.0) {
            sig = make_sig(-1, 0, 0.0, sl, sl * 1.5, 0.0, 0.0, 0.0, 0.0, hold, 2, inst);
        }
        if (b.c < b.o) { st->prev_sw_l = b.l; st->prev_sw_l_rsi = rsi; }
        if (b.c > b.o) { st->prev_sw_h = b.h; st->prev_sw_h_rsi = rsi; }
        return sig;
    }

    case P1_027_SESS_LIQ_OTE_5R: {
        // Asia/London H/L swept -> 1m BOS shift -> OTE 70.5% -> 5R
        double rr = p_at(p, 0, 5.0);
        if (st->sess.asia_ok && st->sw1.bos_up_now && st->sess.asia_l > 0.0 && st->sw1.last_sl < st->sess.asia_l) {
            double sl = std::max(atr, c - st->sw1.last_sl + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, sl * 1.5, inst.tick_size, 90, 4, inst);
        }
        if (st->sess.asia_ok && st->sw1.bos_dn_now && st->sess.asia_h > 0.0 && st->sw1.last_sh > st->sess.asia_h) {
            double sl = std::max(atr, st->sw1.last_sh - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, sl * 1.5, inst.tick_size, 90, 4, inst);
        }
        return {};
    }

    case P1_028_SD_INDECISION: {
        // Supply/Demand indecision candle before >= 2x ATR impulse
        if (st->tf15.just_closed && st->tf15.size() >= 2) {
            const auto& imp = st->tf15.bar(0);
            const auto& ind_c = st->tf15.bar(1);
            if (std::abs(imp.c - imp.o) >= atr * 2.0 && std::abs(ind_c.c - ind_c.o) <= atr * 0.8) {
                st->has_sd_zone = true;
                st->sd_dir = (imp.c > imp.o) ? 1 : -1;
                st->sd_hi = ind_c.h;
                st->sd_lo = ind_c.l;
                st->sd_age = 0;
            }
        }
        if (st->has_sd_zone) {
            double rr = p_at(p, 0, 2.0);
            if (st->sd_dir == 1 && l <= st->sd_hi && c >= st->sd_lo && c > o) {
                st->has_sd_zone = false;
                double sl = std::max(atr, c - st->sd_lo + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            }
            if (st->sd_dir == -1 && h >= st->sd_lo && c <= st->sd_hi && c < o) {
                st->has_sd_zone = false;
                double sl = std::max(atr, st->sd_hi - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            }
        }
        return {};
    }

    case P1_029_915_FVG_V_REC: {
        // 09:15+ ET sweep into FVG -> V-shape recovery -> target London High (2R)
        if (em < 555 || em >= 690) return {};
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->fvg5.find_tap(l, h, 1, false, 12, z) && c > o && (c - o) >= atr * 0.9) {
            double sl = std::max(atr, c - l + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        return {};
    }

    case P1_030_NY_PM_FIRST_FVG: {
        // NY PM Opening Range (13:30-14:00 ET) First Presented FVG
        if (em < 810 || em >= 930) return {};
        if (!st->pm_fvg_captured && em >= 810 && em < 840) {
            if (!st->fvg1.zones.empty() && st->fvg1.zones.front().age == 0) {
                st->pm_fvg = st->fvg1.zones.front();
                st->pm_fvg_captured = true;
            }
        }
        if (st->pm_fvg_captured && em >= 840) {
            double ce = 0.5 * (st->pm_fvg.hi + st->pm_fvg.lo);
            double rr = p_at(p, 0, 2.0);
            if (st->pm_fvg.dir == 1 && l <= ce && c >= st->pm_fvg.lo && c > o) {
                st->pm_fvg_captured = false;
                double sl = std::max(atr, c - st->pm_fvg.lo + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            }
            if (st->pm_fvg.dir == -1 && h >= ce && c <= st->pm_fvg.hi && c < o) {
                st->pm_fvg_captured = false;
                double sl = std::max(atr, st->pm_fvg.hi - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            }
        }
        return {};
    }
    }
    return {};
}

template <int K>
inline void add_p1(std::vector<s546::RegisteredStrategy>& out,
                   const std::string& uid, const std::string& name,
                   const std::string& src, int32_t smask, bool xasset,
                   std::vector<u11::ParamSpec> params) {
    s546::RegisteredStrategy r;
    r.uid = uid;
    r.display_name = name;
    r.super_family = "IG91_Part1_001_030";
    r.source_note = src;
    r.fidelity = "CAUSAL_REEL_SPEC";
    r.needs_cross_asset = xasset;
    r.session_mask = smask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<P1State>(uid, p1_on_bar, p1_init<K>);
    out.push_back(std::move(r));
}

inline void register_ig91_part1(std::vector<s546::RegisteredStrategy>& out) {
    add_p1<P1_001_EMA9_RETEST>(out, "ig91_001_ema9_retest_range", "Reel 001: 9 EMA Retest Range Breakout", "001_DYTCcIcuF7u", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 4.0)});
    add_p1<P1_001_ORBV5_CONF>(out, "ig91_001_orbv5_confluence", "Reel 001: ORBv5 + 9 EMA Retest Confluence", "001_DYTCcIcuF7u", 0xF, false, {p_lin("sl_or_mult", 0.5, 0.25, 1.25), p_lin("rr", 2.5, 1.0, 4.5), p_int("max_entries", 2, 1, 4)});
    add_p1<P1_002_ASIA_IFVG>(out, "ig91_002_asia_ifvg_reversal", "Reel 002: Asia Session Long IFVG Reversal", "002_Dd714BxuQAR", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.5, 4.0)});
    add_p1<P1_003_BREAKER_CHECKLIST>(out, "ig91_003_breaker_checklist", "Reel 003: Breaker Block + Trend MA Checklist", "003_DdhbNwUSwLz", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_004_ENGULFING_50PCT>(out, "ig91_004_engulfing_50pct", "Reel 004: Engulfing 50% Retracement Limit", "004_Dd6cL9lS1zN", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_005_IFVG_AGGR_STOP>(out, "ig91_005_ifvg_aggressive_stop", "Reel 005: ICT FVG Aggressive Stop", "005_DdIZVOwMyvB", 0xF, false, {p_lin("rr", 2.5, 1.2, 4.0)});
    add_p1<P1_006_BIG_LITTLE_BIG>(out, "ig91_006_big_little_big", "Reel 006: Big-Little-Big Impulse Zone", "006_DdepcZfNgJm", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_007_BPR_RETRACE>(out, "ig91_007_bpr_retrace", "Reel 007: Balanced Price Range (BPR) Retrace", "007_DduW3LOAiqC", 0xF, false, {p_lin("rr", 2.5, 1.2, 4.0)});
    add_p1<P1_008_CISD_CONF>(out, "ig91_008_cisd_confirmation", "Reel 008: CISD Entry Confirmation", "008_DdsELscCg7d", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p1<P1_009_REAL_LIQ_SWEEP>(out, "ig91_009_real_liq_sweep", "Reel 009: Real vs Fake Liquidity Sweep (RVOL)", "009_Dduja_Hx5Nx", 0xF, false, {p_lin("rvol_min", 1.3, 1.0, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_010_FOUR_CONF_9PT>(out, "ig91_010_four_conf_9pt_ifvg", "Reel 010: 4-Confluence 5m Sweep + >=9pt IFVG", "010_Dd9-ogRsh_W", 0xF, false, {p_lin("min_ifvg_pts", 9.0, 4.0, 15.0), p_lin("rr", 2.0, 1.2, 4.0)});
    add_p1<P1_011_SWEEP_15M_REJ>(out, "ig91_011_sweep_15m_rejection", "Reel 011: Session Sweep + 15m Rejection Wick", "011_DeAg67yTPsx", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_012_CONSOL_BOS>(out, "ig91_012_consolidation_bos", "Reel 012: 4-Step Consolidation BOS", "012_DeAVbxnJRXO", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_013_FVG_REV_SCALP>(out, "ig91_013_fvg_reversal_scalp", "Reel 013: ICT FVG Reversal Scalp (2R)", "013_DeAdYpzP4Ve", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_014_TRENDS_VP_AMD>(out, "ig91_014_trends_vp_amd", "Reel 014: Trends + VP POC + Distribution", "014_DeAo_Fxiasp", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p1<P1_015_AMD_POC_PULLBACK>(out, "ig91_015_amd_poc_pullback", "Reel 015: Accumulation Manipulation POC Pullback", "015_DeDTS2PI3Yt", 0xF, false, {p_lin("rr", 2.5, 1.2, 4.0), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p1<P1_016_ORB_REJ_RETEST>(out, "ig91_016_orb_rejection_retest", "Reel 016: 15m ORB Rejection Fade", "016_DbcfMyEz-WR", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_017_TDO_SD_RANGES>(out, "ig91_017_tdo_sd_ranges", "Reel 017: True Day Open & 8:30 Equilibrium", "017_DeESZFYRXn1", 0xF, false, {p_lin("sl_atr", 1.8, 1.0, 3.0)});
    add_p1<P1_018_LONDON_SWEEP_50>(out, "ig91_018_london_sweep_50pct", "Reel 018: London Sweep 50% Retrace", "018_DeAygdjsYtQ", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p1<P1_019_OTE_FIB_705>(out, "ig91_019_ote_fib_705", "Reel 019: ICT OTE Fibonacci 70.5% Entry", "019_DeCAiUbgbdg", 0xF, false, {p_lin("rr", 2.0, 1.0, 4.0)});
    add_p1<P1_020_UNICORN_BREAKER>(out, "ig91_020_unicorn_breaker", "Reel 020: Unicorn Model (Breaker + FVG)", "020_Dd9p_NzS2pl", 0xF, false, {p_lin("rr", 2.5, 1.2, 4.0)});
    add_p1<P1_021_SWEEP_IFVG_TL>(out, "ig91_021_sweep_ifvg_trendline", "Reel 021: NQ Sweep + IFVG + EMA Trendline", "021_DddNi7kuCek", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_022_DAILY_DRIFT_LONG>(out, "ig91_022_daily_drift_long", "Reel 022: Daily Drift Long (ATR Trail)", "022_DeHv037uvlD", 0xF, false, {p_lin("sl_atr", 2.5, 1.5, 4.0), p_lin("trig_atr", 1.5, 0.8, 3.0), p_lin("trail_atr", 1.2, 0.5, 2.5)});
    add_p1<P1_023_4H_PD_15M_REJ50>(out, "ig91_023_4h_pd_15m_rej50", "Reel 023: 4H PD Array + 15m 50% Rejection (6R)", "023_DeGgi9whuDy", 0xF, false, {p_lin("sl_pts", 10.0, 5.0, 25.0), p_lin("rr", 6.0, 3.0, 8.0)});
    add_p1<P1_024_30M_VP_POC>(out, "ig91_024_30m_vp_poc_pullback", "Reel 024: 30m VP Breakout + POC Pullback", "024_DeIhVlFiUkU", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p1<P1_025_SESSION_SWITCH>(out, "ig91_025_session_switch", "Reel 025: Session Switch Supply/Demand", "025_DeKGZEiAoSo", 0xF, false, {p_lin("rr", 2.5, 1.2, 4.5), p_lin("sl_pad_atr", 0.5, 0.2, 1.2)});
    add_p1<P1_026_RSI_DIV_5M>(out, "ig91_026_rsi_divergence_5m", "Reel 026: 5m RSI(14) Divergence Short-Hold", "026_DeIyNskR1Wo", 0xF, false, {p_int("hold_bars", 12, 5, 30), p_lin("sl_atr", 2.0, 1.0, 3.5)});
    add_p1<P1_027_SESS_LIQ_OTE_5R>(out, "ig91_027_sess_liq_shift_ote_5r", "Reel 027: Session Liquidity Sweep + Shift + OTE 5R", "027_DbQkSctSgAU", 0xF, false, {p_lin("rr", 5.0, 2.5, 7.0)});
    add_p1<P1_028_SD_INDECISION>(out, "ig91_028_sd_indecision_impulse", "Reel 028: Supply/Demand Indecision Before Impulse", "028_Db6xoS2N-Xf", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_029_915_FVG_V_REC>(out, "ig91_029_915_fvg_v_recovery", "Reel 029: 09:15 AM FVG Sweep + V-Recovery", "029_DeGOdEHt-av", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p1<P1_030_NY_PM_FIRST_FVG>(out, "ig91_030_ny_pm_first_fvg", "Reel 030: NY PM ORB (13:30-14:00) First FVG", "030_DeK7_O7Sv-7", 0xF, false, {p_lin("rr", 2.0, 1.0, 4.0)});
}

} // namespace ig91
} // namespace s546
