#pragma once
#include "ig91_common.hpp"

namespace s546 {
namespace ig91 {

enum P3Kind : int {
    P3_061_IV_WALLS_1 = 0,
    P3_062_SUNDAY_VP_VAH_VAL,
    P3_063_WILLIAMS_R_EMA_VWAP,
    P3_064_HINDI_TREND_BB,
    P3_065_4H_KZ_FVG,
    P3_066_812_912_RANGE_SWEEP,
    P3_067_30M_HS_NECK_RETEST,
    P3_068_PIVOT_DEEP_LADDER,
    P3_069_3M_9EMA_VWAP_CROSS,
    P3_070_STACKED_PO3_10AM,
    P3_071_CE_50_MODEL,
    P3_072_IV_WALLS_90PCT,
    P3_073_ICC_ICT_HTF_OTE,
    P3_074_ACCEPTANCE_HL,
    P3_075_SD_ABSORB_VWAP,
    P3_076_ETH_POC_SHIFT_VA,
    P3_077_TIME_QUARTERLY_90M,
    P3_078_MULTI_CYCLE_TIME,
    P3_079_PURE_TIME_MACROS,
    P3_080_GIANT_CANDLE_5M,
    P3_081_VOL_SPIKE_20EMA_NOWED,
    P3_082_REJ_BODY50_DOUBLE,
    P3_083_FVG_LIQ_BOS_3R,
    P3_084_NQ_DELTA_DIV_VA,
    P3_085_NUCLEAR_TSTAT_TOD,
    P3_086_AMT_FAILED_AUCTION,
    P3_087_MULTI_IND_COMBO,
    P3_088_PD_VAL_SHORT_EXHAUST,
    P3_089_930_OPEN_MANIP,
    P3_090_HINDI_BB_3ELEM,
    P3_091_930_SESSION_SWITCH
};

struct P3State {
    int kind = 0;
    u11::InstrumentSpec inst{};
    std::vector<double> params;

    BarAggregator tf3{}, tf5{}, tf15{}, tf30{}, tf60{}, tf240{};
    FvgTracker fvg1{}, fvg5{}, fvg15{}, fvg240{};
    SwingTracker sw1{}, sw5{}, sw15{}, sw30{};
    VolumeProfileTracker vp_day{}, vp_prev_day{}, vp_eth{}, vp_week{};
    SessionLevelsTracker sess{};
    IndicatorPack ind{};

    int32_t cur_day = -1;
    bool ce50_traded_today = false;
    bool preopen_scanned_today = false;
    PreopenLevels pre{};

    // #085 Nuclear Physics causal minute-of-day rolling stats (390 RTH minutes: 570..959)
    std::array<double, 390> tod_sum{};
    std::array<double, 390> tod_sq{};
    std::array<int, 390> tod_cnt{};
    double prev_min_c = 0.0;
};

template <int K>
inline void p3_init(P3State& st, const std::vector<double>& p, const u11::InstrumentSpec* inst) {
    st = P3State{};
    st.kind = K;
    st.inst = inst ? *inst : u11::get_instrument_spec("NQ");
    st.params = p;
    st.tf3.reset(3, 24);
    st.tf5.reset(5, 24);
    st.tf15.reset(15, 24);
    st.tf30.reset(30, 24);
    st.tf60.reset(60, 24);
    st.tf240.reset(240, 16);
    st.fvg1.reset(12);
    st.fvg5.reset(12);
    st.fvg15.reset(12);
    st.fvg240.reset(8);
    st.sw1.reset(3);
    st.sw5.reset(2);
    st.sw15.reset(2);
    st.sw30.reset(2);
    double bpts = std::max(1.0, 5.0 * sym_pt_scale(st.inst));
    st.vp_day.reset(bpts);
    st.vp_prev_day.reset(bpts);
    st.vp_eth.reset(bpts);
    st.vp_week.reset(bpts);
    st.ind.reset();
}

inline u11::PluginSignal p3_on_bar(const u11::CausalBarView& v, void* state_ptr) {
    auto* st = static_cast<P3State*>(state_ptr);
    const auto& p = st->params;
    const auto& inst = st->inst;

    st->tf3.update(v);
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
        if (st->cur_day > 0) {
            int32_t gap = ymd_to_days(ymd) - ymd_to_days(st->cur_day);
            if (gap >= 2 || ymd_dow(ymd) == 1) {
                st->vp_week.reset(st->vp_day.bin_pts);
            }
            st->vp_day.compute();
            st->vp_prev_day = st->vp_day;
            st->vp_day.reset(st->vp_day.bin_pts);
            st->vp_eth.reset(st->vp_day.bin_pts);
        }
        st->cur_day = ymd;
        st->ce50_traded_today = false;
        st->preopen_scanned_today = false;
        st->pre = PreopenLevels{};
    }

    if (em >= 570 && em < 960 && !st->preopen_scanned_today) {
        st->pre = scan_preopen_levels(v, atr);
        st->preopen_scanned_today = true;
    }
    if (em >= 570 && st->preopen_scanned_today && st->pre.pm812_ok) {
        if (l < st->pre.pm812_l) st->pre.pm812_swept_dir = 1;
        else if (h > st->pre.pm812_h) st->pre.pm812_swept_dir = -1;
    }

    // Causal TOD return update for #085
    if (st->kind == P3_085_NUCLEAR_TSTAT_TOD && em >= 570 && em < 960 && st->prev_min_c > 0.0) {
        int m_idx = em - 570;
        double ret = c - st->prev_min_c;
        st->tod_sum[m_idx] += ret;
        st->tod_sq[m_idx] += ret * ret;
        st->tod_cnt[m_idx]++;
    }
    st->prev_min_c = c;

    st->vp_day.add_bar(h, l, c, vol);
    st->vp_week.add_bar(h, l, c, vol);
    if (em < 570 || em >= 960) st->vp_eth.add_bar(h, l, c, vol);
    if (v.bar_index() % 5 == 0) {
        st->vp_day.compute();
        st->vp_eth.compute();
        st->vp_week.compute();
    }

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
    if (st->tf30.just_closed) {
        st->sw30.update(st->tf30.bar(0));
    }
    if (st->tf240.just_closed) {
        st->fvg240.on_closed_bar(st->tf240.bar(0), st->tf240.bar(1), st->tf240.bar(2), inst.tick_size);
    }

    if (v.bar_index() < 30 || v.in_position) return {};

    switch (st->kind) {
    case P3_061_IV_WALLS_1:
    case P3_072_IV_WALLS_90PCT: {
        // OHLCV Realized-Vol IV Wall Reversal
        if (em < 570 || em >= 930 || st->ind.rv_wall_hi <= st->ind.rv_wall_lo) return {};
        double mult = p_at(p, 0, 1.0);
        double mid = st->ind.day_open;
        double hw = (st->ind.rv_wall_hi - mid) * mult;
        double wall_hi = mid + hw, wall_lo = mid - hw;
        double sl = atr * p_at(p, 1, 2.0), rr = p_at(p, 2, 2.5);
        if (h >= wall_hi && c < o) return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        if (l <= wall_lo && c > o) return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        return {};
    }

    case P3_062_SUNDAY_VP_VAH_VAL: {
        if (!st->vp_week.valid) return {};
        double sl = atr * p_at(p, 0, 1.8), rr = p_at(p, 1, 2.5);
        if (l <= st->vp_week.val && c > st->vp_week.val && c > o && st->vp_week.is_lvn(st->vp_week.val, 0.85))
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        if (h >= st->vp_week.vah && c < st->vp_week.vah && c < o && st->vp_week.is_lvn(st->vp_week.vah, 0.85))
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P3_063_WILLIAMS_R_EMA_VWAP: {
        // Williams %R < -80, Close > EMA100, Close < 0.99 * VWAP -> Long mean reversion (RTH gated)
        if (em < 570 || em >= 960 || v.sess_entries >= 2 || st->ind.count < 100) return {};
        double vw = v.session_vwap();
        double disc = p_at(p, 0, 0.995);
        double wr_th = p_at(p, 1, -80.0);
        if (c > st->ind.ema100 && st->ind.williams_r < wr_th && c < vw * disc) {
            double sl = atr * p_at(p, 2, 2.0);
            return make_sig(1, 0, 0.0, sl, std::max(sl * 1.5, vw - c), 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        }
        return {};
    }

    case P3_064_HINDI_TREND_BB:
    case P3_090_HINDI_BB_3ELEM: {
        // Strong Trend + Bollinger Bands (20, 2.0) + Volume
        if (st->ind.bb_up <= st->ind.bb_dn) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (v.ema9() > v.ema20() && v.ema20() > v.ema50() && c > st->ind.bb_mid && static_cast<double>(v[-1].c) <= st->ind.bb_mid && v.rel_vol() >= 1.1)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (v.ema9() < v.ema20() && v.ema20() < v.ema50() && c < st->ind.bb_mid && static_cast<double>(v[-1].c) >= st->ind.bb_mid && v.rel_vol() >= 1.1)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_065_4H_KZ_FVG: {
        if (em < 570 || em >= 720) return {};
        FvgZone z;
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.5);
        if (v.ema20() > v.ema50() && st->fvg15.find_tap(l, h, 1, false, 16, z) && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (v.ema20() < v.ema50() && st->fvg15.find_tap(l, h, -1, false, 16, z) && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_066_812_912_RANGE_SWEEP: {
        // 8:12-9:12 AM ET range sweep (scanned from causal past bars) -> target opposite end in NY_AM
        if (!st->pre.pm812_ok || em < 570 || em >= 720 || v.sess_entries >= 2) return {};
        double rng = st->pre.pm812_h - st->pre.pm812_l;
        if (rng < atr * 1.5) return {};
        double sl = atr * p_at(p, 0, 1.5);
        if (st->pre.pm812_swept_dir == 1 && c > st->pre.pm812_l && c > o) {
            st->pre.pm812_swept_dir = 0;
            return make_sig(1, 0, 0.0, sl, std::max(sl * 1.5, st->pre.pm812_h - c), 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        }
        if (st->pre.pm812_swept_dir == -1 && c < st->pre.pm812_h && c < o) {
            st->pre.pm812_swept_dir = 0;
            return make_sig(-1, 0, 0.0, sl, std::max(sl * 1.5, c - st->pre.pm812_l), 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        }
        return {};
    }

    case P3_067_30M_HS_NECK_RETEST: {
        if (!st->tf30.just_closed) return {};
        double sl = atr * p_at(p, 0, 2.0), rr = p_at(p, 1, 2.0);
        if (st->sw30.bos_dn_now) return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        if (st->sw30.bos_up_now) return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P3_068_PIVOT_DEEP_LADDER: {
        // Deep pullback ladder in trend direction with volatility SL
        double dip_atr = p_at(p, 0, 1.5);
        double sl = atr * p_at(p, 1, 2.0), rr = p_at(p, 2, 2.0);
        if (v.ema20() > v.ema50() && c < v.ema20() - atr * dip_atr && c > o && st->ind.rsi14 < 40.0)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (v.ema20() < v.ema50() && c > v.ema20() + atr * dip_atr && c < o && st->ind.rsi14 > 60.0)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_069_3M_9EMA_VWAP_CROSS: {
        if (!st->tf3.just_closed) return {};
        const auto& b = st->tf3.bar(0);
        double vw = v.session_vwap(-1), e9 = v.ema9(-1);
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (b.o < vw && b.c > vw && b.c > e9) return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
        if (b.o > vw && b.c < vw && b.c < e9) return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
        return {};
    }

    case P3_070_STACKED_PO3_10AM: {
        // 10:00 AM stacked PO3: 15m manipulation wick into liquidity -> expansion
        if (em < 600 || em >= 690 || !st->tf15.just_closed) return {};
        const auto& b = st->tf15.bar(0);
        double rng = std::max(inst.tick_size, b.h - b.l);
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.5);
        if ((std::min(b.o, b.c) - b.l) / rng >= 0.40 && b.c > b.o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        if ((b.h - std::max(b.o, b.c)) / rng >= 0.40 && b.c < b.o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        return {};
    }

    case P3_071_CE_50_MODEL: {
        // 8:30-9:30 H/L split at 50%; first 3m candle after 9:30 closes above/below -> enter & trail
        if (!st->pre.ce50_ok || st->ce50_traded_today || em < 573 || em >= 630) return {};
        if (!st->tf3.just_closed) return {};
        const auto& b = st->tf3.bar(0);
        double mid = 0.5 * (st->pre.ce50_hi + st->pre.ce50_lo);
        st->ce50_traded_today = true;
        if (b.c > mid) {
            double sl = std::max(atr * 1.5, c - st->pre.ce50_lo);
            return make_sig(1, 0, 0.0, sl, sl * p_at(p, 0, 3.0), sl * 0.8, sl * 0.4, 0.0, 0.0, 360, 5, inst);
        }
        if (b.c < mid) {
            double sl = std::max(atr * 1.5, st->pre.ce50_hi - c);
            return make_sig(-1, 0, 0.0, sl, sl * p_at(p, 0, 3.0), sl * 0.8, sl * 0.4, 0.0, 0.0, 360, 5, inst);
        }
        return {};
    }

    case P3_073_ICC_ICT_HTF_OTE: {
        if (!st->sw15.has_sh || !st->sw15.has_sl) return {};
        double rng = st->sw15.last_sh - st->sw15.last_sl;
        if (rng < atr * 2.0) return {};
        double rr = p_at(p, 0, 2.5);
        if (st->sw15.trend_dir == 1 && l <= st->sw15.last_sh - 0.65 * rng && c > o) {
            double sl = std::max(atr, c - st->sw15.last_sl + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        }
        if (st->sw15.trend_dir == -1 && h >= st->sw15.last_sl + 0.65 * rng && c < o) {
            double sl = std::max(atr, st->sw15.last_sh - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        }
        return {};
    }

    case P3_074_ACCEPTANCE_HL: {
        // 15m/1H Higher-Low BOS acceptance pullback
        if (!st->sw15.has_sl || !st->sw15.has_sh) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (st->sw15.trend_dir == 1 && l <= st->sw15.last_sl + atr * 0.5 && c > st->sw15.last_sl && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (st->sw15.trend_dir == -1 && h >= st->sw15.last_sh - atr * 0.5 && c < st->sw15.last_sh && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_075_SD_ABSORB_VWAP: {
        // Volume absorption (high rel_vol + small body) -> target VWAP
        double vw = v.session_vwap();
        double dist = std::abs(c - vw);
        if (v.rel_vol() >= p_at(p, 0, 1.5) && std::abs(c - o) <= 0.35 * std::max(inst.tick_size, h - l) && dist >= atr * 1.5) {
            double sl = atr * p_at(p, 1, 1.5);
            if (c < vw) return make_sig(1, 0, 0.0, sl, dist * 0.9, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            if (c > vw) return make_sig(-1, 0, 0.0, sl, dist * 0.9, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P3_076_ETH_POC_SHIFT_VA: {
        if (!st->vp_eth.valid || !st->vp_prev_day.valid) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (st->vp_eth.poc > st->vp_prev_day.poc && l <= st->vp_eth.val && c > st->vp_eth.val && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (st->vp_eth.poc < st->vp_prev_day.poc && h >= st->vp_eth.vah && c < st->vp_eth.vah && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_077_TIME_QUARTERLY_90M:
    case P3_078_MULTI_CYCLE_TIME:
    case P3_079_PURE_TIME_MACROS: {
        // Time-based macro windows: 09:30, 10:00, 10:50, 13:30, 14:50 ET
        bool in_macro = (em >= 570 && em <= 580) || (em >= 595 && em <= 610) || (em >= 650 && em <= 670) || (em >= 810 && em <= 825);
        if (!in_macro) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (v.rel_vol() >= 1.2 && c > o && c > v.ema9())
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 5, inst);
        if (v.rel_vol() >= 1.2 && c < o && c < v.ema9())
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 5, inst);
        return {};
    }

    case P3_080_GIANT_CANDLE_5M: {
        // 5m candle >= 2x ATR closing through 15m swing H/L
        if (!st->tf5.just_closed || !st->sw15.has_sh || !st->sw15.has_sl) return {};
        const auto& b = st->tf5.bar(0);
        if ((b.h - b.l) < atr * p_at(p, 0, 2.0)) return {};
        double sl = atr * p_at(p, 1, 2.0);
        if (b.c > st->sw15.last_sh) return make_sig(1, 0, 0.0, sl, 0.0, sl * 1.0, sl * 0.5, 0.0, 0.0, 240, 5, inst);
        if (b.c < st->sw15.last_sl) return make_sig(-1, 0, 0.0, sl, 0.0, sl * 1.0, sl * 0.5, 0.0, 0.0, 240, 5, inst);
        return {};
    }

    case P3_081_VOL_SPIKE_20EMA_NOWED: {
        // Volume spike breakout + 20-EMA agreement, skip Wednesday (dow==3)
        if (ymd_dow(ymd) == 3) return {};
        double rvol_th = p_at(p, 0, 1.8), sl = atr * p_at(p, 1, 2.0), rr = p_at(p, 2, 3.0);
        if (v.rel_vol() >= rvol_th) {
            if (c > o && c > v.ema20() && v.ema20() > v.ema50())
                return make_sig(1, 0, 0.0, sl, sl * rr, sl * 1.0, sl * 0.5, 0.0, 0.0, 120, 5, inst);
            if (c < o && c < v.ema20() && v.ema20() < v.ema50())
                return make_sig(-1, 0, 0.0, sl, sl * rr, sl * 1.0, sl * 0.5, 0.0, 0.0, 120, 5, inst);
        }
        return {};
    }

    case P3_082_REJ_BODY50_DOUBLE: {
        // Bullish/Bearish candle with >50% body + rejection wick
        double rng = std::max(inst.tick_size, h - l);
        double body = std::abs(c - o);
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (c > o && body / rng >= 0.50 && (o - l) / rng >= 0.25 && st->sw1.sweep_lo_now)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (c < o && body / rng >= 0.50 && (h - o) / rng >= 0.25 && st->sw1.sweep_hi_now)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_083_FVG_LIQ_BOS_3R: {
        FvgZone z;
        double rr = p_at(p, 0, 3.0);
        if (st->sw1.trend_dir == 1 && st->fvg1.find_tap(l, h, 1, false, 15, z) && c > o) {
            double sl = std::max(atr, c - z.lo + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (st->sw1.trend_dir == -1 && st->fvg1.find_tap(l, h, -1, false, 15, z) && c < o) {
            double sl = std::max(atr, z.hi - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P3_084_NQ_DELTA_DIV_VA: {
        // CVD proxy divergence at prior RTH/ETH VAH/VAL
        if (!st->vp_prev_day.valid) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (l <= st->vp_prev_day.val && st->ind.cvd > st->ind.prev_cvd && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (h >= st->vp_prev_day.vah && st->ind.cvd < st->ind.prev_cvd && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_085_NUCLEAR_TSTAT_TOD: {
        // Causal rolling t-stat anomaly of RTH minute-of-day
        if (em < 570 || em >= 955) return {};
        int m_idx = em - 570;
        int cnt = st->tod_cnt[m_idx];
        if (cnt < 20) return {};
        double mean = st->tod_sum[m_idx] / static_cast<double>(cnt);
        double var = std::max(1e-9, st->tod_sq[m_idx] / static_cast<double>(cnt) - mean * mean);
        double tstat = mean / std::sqrt(var / static_cast<double>(cnt));
        double z_th = p_at(p, 0, 2.0), sl = atr * p_at(p, 1, 1.5);
        if (tstat > z_th) return make_sig(1, 0, 0.0, sl, sl * 1.5, 0.0, 0.0, 0.0, 0.0, 15, 2, inst);
        if (tstat < -z_th) return make_sig(-1, 0, 0.0, sl, sl * 1.5, 0.0, 0.0, 0.0, 0.0, 15, 2, inst);
        return {};
    }

    case P3_086_AMT_FAILED_AUCTION: {
        if (!st->vp_prev_day.valid || em < 570 || em >= 840) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (static_cast<double>(v[-1].h) > st->vp_prev_day.vah && c < st->vp_prev_day.vah && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        if (static_cast<double>(v[-1].l) < st->vp_prev_day.val && c > st->vp_prev_day.val && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        return {};
    }

    case P3_087_MULTI_IND_COMBO: {
        // RSI>50 + Price>EMA20 + Price>VWAP + Stoch K>D + Price>KAMA + Williams %R > -20
        double sl = atr * p_at(p, 0, 1.8), rr = p_at(p, 1, 2.0);
        if (st->ind.rsi14 > 50.0 && c > v.ema20() && c > v.session_vwap() && st->ind.stoch_k > st->ind.stoch_d && c > st->ind.kama && st->ind.williams_r > -20.0)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (st->ind.rsi14 < 50.0 && c < v.ema20() && c < v.session_vwap() && st->ind.stoch_k < st->ind.stoch_d && c < st->ind.kama && st->ind.williams_r < -80.0)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_088_PD_VAL_SHORT_EXHAUST: {
        if (!st->vp_prev_day.valid || em < 570 || em >= 840) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (c < st->vp_prev_day.val && c < o && st->ind.cvd < st->ind.prev_cvd)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P3_089_930_OPEN_MANIP: {
        // Pre-9:30 FVG tap/retrace -> distribute opposite at 9:30-9:45
        if (em < 570 || em > 585) return {};
        FvgZone z;
        double sl = atr * p_at(p, 0, 1.8), rr = p_at(p, 1, 2.5);
        if (st->fvg5.find_tap(l, h, 1, false, 12, z) && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        if (st->fvg5.find_tap(l, h, -1, false, 12, z) && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        return {};
    }

    case P3_091_930_SESSION_SWITCH: {
        // 9:30 AM exact session switch reversal from IFVG/supply-demand zone
        if (em < 568 || em > 578) return {};
        FvgZone z;
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.5);
        if (st->fvg1.find_tap(l, h, 1, true, 15, z) && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        if (st->fvg1.find_tap(l, h, -1, true, 15, z) && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        return {};
    }
    }
    return {};
}

template <int K>
inline void add_p3(std::vector<s546::RegisteredStrategy>& out,
                   const std::string& uid, const std::string& name,
                   const std::string& src, int32_t smask, bool xasset,
                   std::vector<u11::ParamSpec> params) {
    s546::RegisteredStrategy r;
    r.uid = uid;
    r.display_name = name;
    r.super_family = "IG91_Part3_061_091";
    r.source_note = src;
    r.fidelity = "CAUSAL_REEL_SPEC";
    r.needs_cross_asset = xasset;
    r.session_mask = smask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<P3State>(uid, p3_on_bar, p3_init<K>);
    out.push_back(std::move(r));
}

inline void register_ig91_part3(std::vector<s546::RegisteredStrategy>& out) {
    add_p3<P3_061_IV_WALLS_1>(out, "ig91_061_iv_walls_reversal", "Reel 061: IV Walls Reversal (RV Proxy)", "061_Deu7J2YvDen", 0xF, false, {p_lin("wall_mult", 1.0, 0.6, 1.8), p_lin("sl_atr", 2.0, 1.0, 3.0), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_062_SUNDAY_VP_VAH_VAL>(out, "ig91_062_sunday_vp_vah_val", "Reel 062: Sunday-Anchored Week VP VAH/VAL", "062_DepUx5Fp2rl", 0xF, false, {p_lin("sl_atr", 1.8, 1.0, 3.0), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_063_WILLIAMS_R_EMA_VWAP>(out, "ig91_063_williams_r_ema_vwap", "Reel 063: Williams %R + EMA100 + VWAP", "063_DetWSY5DXy_", 0xF, false, {p_lin("vwap_disc", 0.995, 0.985, 0.999), p_lin("wr_th", -80.0, -95.0, -65.0), p_lin("sl_atr", 2.0, 1.0, 3.5)});
    add_p3<P3_064_HINDI_TREND_BB>(out, "ig91_064_hindi_trend_bb", "Reel 064: Strong Trend + Bollinger Bands", "064_DeyGTGyILby", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_065_4H_KZ_FVG>(out, "ig91_065_4h_kz_fvg", "Reel 065: Trend + Killzone + FVG", "065_DevOs-stFBh", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_066_812_912_RANGE_SWEEP>(out, "ig91_066_812_912_range_sweep", "Reel 066: 8:12-9:12 AM Range Sweep", "066_DezNYoKNN2g", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p3<P3_067_30M_HS_NECK_RETEST>(out, "ig91_067_30m_hs_neck_retest", "Reel 067: 30m H&S Neckline Break", "067_De1mGvltFqZ", 0xF, false, {p_lin("sl_atr", 2.0, 1.0, 3.0), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_068_PIVOT_DEEP_LADDER>(out, "ig91_068_pivot_deep_ladder", "Reel 068: Pivot Deep Ladder", "068_De1TEFSjOd-", 0xF, false, {p_lin("dip_atr", 1.5, 0.8, 2.5), p_lin("sl_atr", 2.0, 1.0, 3.0), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_069_3M_9EMA_VWAP_CROSS>(out, "ig91_069_3m_9ema_vwap_cross", "Reel 069: 3m 9-EMA + VWAP Cross", "069_De5LrCnAzL9", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_070_STACKED_PO3_10AM>(out, "ig91_070_stacked_po3_10am", "Reel 070: Stacked PO3 10:00 AM", "070_De6nfKVielt", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_071_CE_50_MODEL>(out, "ig91_071_ce_50_model", "Reel 071: CE 50% Model (8:30-9:30)", "071_DeXrGH6PYzx", 0xF, false, {p_lin("rr", 3.0, 1.5, 6.0)});
    add_p3<P3_072_IV_WALLS_90PCT>(out, "ig91_072_iv_walls_90pct", "Reel 072: IV Walls 90% Range (RV Proxy)", "072_DfFkkM5z7Mm", 0xF, false, {p_lin("wall_mult", 1.0, 0.6, 1.8), p_lin("sl_atr", 2.0, 1.0, 3.0), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_073_ICC_ICT_HTF_OTE>(out, "ig91_073_icc_ict_htf_ote", "Reel 073: ICC + ICT HTF BOS + OTE", "073_Ddc_9GFtiHm", 0xF, false, {p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_074_ACCEPTANCE_HL>(out, "ig91_074_acceptance_hl", "Reel 074: Acceptance Higher-Low Pullback", "074_DZp0mfgRIWj", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_075_SD_ABSORB_VWAP>(out, "ig91_075_sd_absorb_vwap", "Reel 075: Volume Absorption -> VWAP", "075_1PVWzGKrK6", 0xF, false, {p_lin("rvol_min", 1.5, 1.0, 2.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p3<P3_076_ETH_POC_SHIFT_VA>(out, "ig91_076_eth_poc_shift_va", "Reel 076: ETH POC Shift + VA Framing", "076_DfXnCgJzq2B", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_077_TIME_QUARTERLY_90M>(out, "ig91_077_time_quarterly_90m", "Reel 077: Quarterly 90m Time Cycle", "077_Dfb_fvghTz8", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_078_MULTI_CYCLE_TIME>(out, "ig91_078_multi_cycle_time", "Reel 078: Multi-Cycle Time Alignment", "078_Dfegu6rh9RJ", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_079_PURE_TIME_MACROS>(out, "ig91_079_pure_time_macros", "Reel 079: Pure Time-of-Day Macros", "079_DfegftOBn8t", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_080_GIANT_CANDLE_5M>(out, "ig91_080_giant_candle_5m", "Reel 080: Giant 5m Candle (>=2x ATR) BOS", "080_DfYnKt5NfS-", 0xF, false, {p_lin("giant_mult", 2.0, 1.4, 3.0), p_lin("sl_atr", 2.0, 1.0, 3.0)});
    add_p3<P3_081_VOL_SPIKE_20EMA_NOWED>(out, "ig91_081_vol_spike_20ema_nowed", "Reel 081: Volume Spike + 20-EMA (Skip Wed)", "081_DfgRJ9aNNeo", 0xF, false, {p_lin("rvol_th", 1.8, 1.2, 3.0), p_lin("sl_atr", 2.0, 1.0, 3.0), p_lin("rr", 3.0, 1.5, 5.0)});
    add_p3<P3_082_REJ_BODY50_DOUBLE>(out, "ig91_082_rej_body50_double", "Reel 082: >50% Body + Rejection Wick", "082_Dffg3wDIzxR", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_083_FVG_LIQ_BOS_3R>(out, "ig91_083_fvg_liq_bos_3r", "Reel 083: FVG + Liquidity + BOS (3R)", "083_1D1wPQhVGN", 0xF, false, {p_lin("rr", 3.0, 1.5, 4.5)});
    add_p3<P3_084_NQ_DELTA_DIV_VA>(out, "ig91_084_nq_delta_div_va", "Reel 084: NQ CVD Proxy Divergence at VA", "084_DfsndKMTiVV", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_085_NUCLEAR_TSTAT_TOD>(out, "ig91_085_nuclear_tstat_tod", "Reel 085: Nuclear Physics TOD T-Stat", "085_DfNIZGlAF_c", 0xF, false, {p_lin("z_th", 2.0, 1.2, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p3<P3_086_AMT_FAILED_AUCTION>(out, "ig91_086_amt_failed_auction", "Reel 086: AMT Failed Auction VAH/VAL", "086_DfuwA3eT4wm", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_087_MULTI_IND_COMBO>(out, "ig91_087_multi_ind_combo", "Reel 087: 6-Indicator Confluence Combo", "087_DfP5vCwjQMR", 0xF, false, {p_lin("sl_atr", 1.8, 1.0, 3.0), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_088_PD_VAL_SHORT_EXHAUST>(out, "ig91_088_pd_val_short_exhaust", "Reel 088: PD Cash VAL Short + Exhaustion", "088_Df2XgJHT41y", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_089_930_OPEN_MANIP>(out, "ig91_089_930_open_manip", "Reel 089: 9:30 Open Manipulation FVG", "089_DfsDSnPAbT4", 0xF, false, {p_lin("sl_atr", 1.8, 1.0, 3.0), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p3<P3_090_HINDI_BB_3ELEM>(out, "ig91_090_hindi_bb_3elem", "Reel 090: Bollinger Bands 3-Element Breakout", "090_Df6RujzI8sI", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p3<P3_091_930_SESSION_SWITCH>(out, "ig91_091_930_session_switch", "Reel 091: 9:30 Exact Session Switch IFVG", "091_Df5Mj_4Asz7", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.2, 4.0)});
}

} // namespace ig91
} // namespace s546
