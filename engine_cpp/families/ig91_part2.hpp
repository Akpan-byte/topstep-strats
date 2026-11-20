#pragma once
#include "ig91_common.hpp"

namespace s546 {
namespace ig91 {

enum P2Kind : int {
    P2_031_ORB_QUANT = 0,
    P2_032_S1_PD_DOUBLE,
    P2_032_S2_PM_DOUBLE,
    P2_032_S3_INTRADAY_DOUBLE,
    P2_032_S4_VWAP_REV,
    P2_032_S5_EMA200_REV,
    P2_032_COMBINED,
    P2_033_EMA_VWAP_CROSS,
    P2_034_SHORT_NQ_GAMMA_CEIL,
    P2_035_SHORT_ABP_FVG,
    P2_036_OR_FAKEOUT_FLOW,
    P2_037_30M_AMD_SMT_CISD,
    P2_038_VP_OVERLAP_SD,
    P2_039_8AM_9AM_SWEEP_DISP,
    P2_040_REJ_BLOCK_50_IFVG,
    P2_041_4STEP_FILTER_CISD,
    P2_042_MTF_HS_BOS_EMA,
    P2_043_ONE_CANDLE_6R,
    P2_044_REJ_BLOCK_96WR,
    P2_045_8AM_SWEEP_3M_IFVG,
    P2_046_OMAR_NOWICK,
    P2_047_15M_SWEEP_2M_FVG,
    P2_048_CLOSE_SWEEP_FVG,
    P2_049_ICT_OTE_79,
    P2_050_EVP_TYPE1_FRONTSIDE,
    P2_050_EVP_TYPE2_CONFIRM,
    P2_050_EVP_COMBINED,
    P2_051_4H_PRED_STOPRUN,
    P2_052_830_NEWS_CONT_FADE,
    P2_053_ASIA_LON_NY_INV,
    P2_054_OVN_VP_BREAKOUT,
    P2_055_ASIA_HUNT_NY_LATE,
    P2_056_OVN_ACCUM_1H_OB,
    P2_057_PD_PW_HVN_REACT,
    P2_058_LON_ASIA_SWEEP_IFVG,
    P2_059_LVN_LOD_RECLAIM,
    P2_060_LVN_WICK_AGGR,
    P2_059_060_FILTER_ORB,
    P2_059_060_FILTER_EMA_VWAP
};

struct P2State {
    int kind = 0;
    u11::InstrumentSpec inst{};
    std::vector<double> params;

    BarAggregator tf2{}, tf3{}, tf5{}, tf15{}, tf30{}, tf60{}, tf240{};
    FvgTracker fvg1{}, fvg3{}, fvg15{}, fvg240{};
    SwingTracker sw1{}, sw3{}, sw15{};
    VolumeProfileTracker vp_day{}, vp_prev_day{}, vp_ovn{}, vp_week{};
    SessionLevelsTracker sess{};
    IndicatorPack ind{};

    int32_t cur_day = -1;
    double prev_rth_h = 0.0, prev_rth_l = 0.0;
    bool prev_rth_ok = false;
    int8_t or_fakeout_dir = 0;
    int or_retest_cnt = 0;
    bool preopen_scanned_today = false;
    bool news_dir_taken = false;
    PreopenLevels pre{};

    // EVP Type 2 pending confirmation
    int8_t pending_evp_dir = 0;
    double pending_evp_sl_px = 0.0;
    int pending_evp_age = 0;
};

template <int K>
inline void p2_init(P2State& st, const std::vector<double>& p, const u11::InstrumentSpec* inst) {
    st = P2State{};
    st.kind = K;
    st.inst = inst ? *inst : u11::get_instrument_spec("NQ");
    st.params = p;
    st.tf2.reset(2, 24);
    st.tf3.reset(3, 24);
    st.tf5.reset(5, 24);
    st.tf15.reset(15, 24);
    st.tf30.reset(30, 24);
    st.tf60.reset(60, 24);
    st.tf240.reset(240, 16);
    st.fvg1.reset(12);
    st.fvg3.reset(12);
    st.fvg15.reset(12);
    st.fvg240.reset(8);
    st.sw1.reset(3);
    st.sw3.reset(2);
    st.sw15.reset(2);
    double bpts = std::max(1.0, 5.0 * sym_pt_scale(st.inst));
    st.vp_day.reset(bpts);
    st.vp_prev_day.reset(bpts);
    st.vp_ovn.reset(bpts);
    st.vp_week.reset(bpts);
    st.ind.reset();
}

inline u11::PluginSignal eval_032_sub(int sub_idx, const P2State& st, const u11::CausalBarView& v, double atr) {
    double c = static_cast<double>(v[0].c);
    double o = static_cast<double>(v[0].o);
    double h = static_cast<double>(v[0].h);
    double l = static_cast<double>(v[0].l);
    double tol = atr * 0.6;
    double sl = atr * p_at(st.params, 0, 1.5);
    double rr = p_at(st.params, 1, 2.0);
    const auto& inst = st.inst;

    if (sub_idx == 1 && v.has_prior_day()) {
        if (h >= v.pdh() - tol && static_cast<double>(v[-1].h) >= v.pdh() - tol && c < o && c < v.pdh())
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        if (l <= v.pdl() + tol && static_cast<double>(v[-1].l) <= v.pdl() + tol && c > o && c > v.pdl())
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
    }
    if (sub_idx == 2 && st.sess.lon_ok) {
        if (h >= st.sess.lon_h - tol && static_cast<double>(v[-1].h) >= st.sess.lon_h - tol && c < o && c < st.sess.lon_h)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        if (l <= st.sess.lon_l + tol && static_cast<double>(v[-1].l) <= st.sess.lon_l + tol && c > o && c > st.sess.lon_l)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
    }
    if (sub_idx == 3 && v.et_mins() >= 600 && st.prev_rth_ok) {
        if (h >= st.prev_rth_h - tol && static_cast<double>(v[-1].h) >= st.prev_rth_h - tol && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        if (l <= st.prev_rth_l + tol && static_cast<double>(v[-1].l) <= st.prev_rth_l + tol && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
    }
    if (sub_idx == 4) {
        double vw = v.session_vwap();
        if (l <= vw && c > vw && c > o && static_cast<double>(v[-1].c) < vw)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        if (h >= vw && c < vw && c < o && static_cast<double>(v[-1].c) > vw)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
    }
    if (sub_idx == 5 && st.ind.count > 200) {
        double e200 = st.ind.ema200;
        if (l <= e200 && c > e200 && c > o && static_cast<double>(v[-1].c) < e200)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        if (h >= e200 && c < e200 && c < o && static_cast<double>(v[-1].c) > e200)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
    }
    return {};
}

inline u11::PluginSignal p2_on_bar(const u11::CausalBarView& v, void* state_ptr) {
    auto* st = static_cast<P2State*>(state_ptr);
    const auto& p = st->params;
    const auto& inst = st->inst;

    st->tf2.update(v);
    st->tf3.update(v);
    st->tf5.update(v);
    st->tf15.update(v);
    st->tf30.update(v);
    st->tf60.update(v);
    st->tf240.update(v);
    st->prev_rth_h = st->sess.rth_h;
    st->prev_rth_l = st->sess.rth_l;
    st->prev_rth_ok = st->sess.rth_ok;
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
            st->vp_ovn.reset(st->vp_day.bin_pts);
        }
        st->cur_day = ymd;
        st->or_fakeout_dir = 0;
        st->or_retest_cnt = 0;
        st->preopen_scanned_today = false;
        st->news_dir_taken = false;
        st->pre = PreopenLevels{};
    }

    st->vp_day.add_bar(h, l, c, vol);
    st->vp_week.add_bar(h, l, c, vol);
    if (em >= 990 || em < 570) st->vp_ovn.add_bar(h, l, c, vol);
    if (v.bar_index() % 5 == 0) {
        st->vp_day.compute();
        st->vp_ovn.compute();
        st->vp_week.compute();
    }

    AggBar b1{o, h, l, c, vol, static_cast<int64_t>(v.bar_index()), true};
    AggBar b1_1{static_cast<double>(v[-1].o), static_cast<double>(v[-1].h), static_cast<double>(v[-1].l), static_cast<double>(v[-1].c), static_cast<double>(v[-1].v), 0, v.bar_index() >= 1};
    AggBar b1_2{static_cast<double>(v[-2].o), static_cast<double>(v[-2].h), static_cast<double>(v[-2].l), static_cast<double>(v[-2].c), static_cast<double>(v[-2].v), 0, v.bar_index() >= 2};
    st->fvg1.on_closed_bar(b1, b1_1, b1_2, inst.tick_size);
    st->sw1.update(b1);

    if (st->tf3.just_closed) {
        st->fvg3.on_closed_bar(st->tf3.bar(0), st->tf3.bar(1), st->tf3.bar(2), inst.tick_size);
        st->sw3.update(st->tf3.bar(0));
    }
    if (st->tf15.just_closed) {
        st->fvg15.on_closed_bar(st->tf15.bar(0), st->tf15.bar(1), st->tf15.bar(2), inst.tick_size);
        st->sw15.update(st->tf15.bar(0));
    }
    if (st->tf240.just_closed) {
        st->fvg240.on_closed_bar(st->tf240.bar(0), st->tf240.bar(1), st->tf240.bar(2), inst.tick_size);
    }
    if (st->pending_evp_dir != 0) {
        st->pending_evp_age++;
        if (st->pending_evp_age > 30) st->pending_evp_dir = 0;
    }

    // Scan 07:30-09:29 ET causal past bars on first NY_AM bar (since 06:00-09:29 has sess_id == -1)
    if (em >= 570 && em < 960 && !st->preopen_scanned_today) {
        st->pre = scan_preopen_levels(v, atr);
        st->preopen_scanned_today = true;
    }
    if (em >= 570 && st->preopen_scanned_today) {
        if (st->pre.h9_ok) {
            if (l < st->pre.h9_l) st->pre.h9_swept_dir = 1;
            else if (h > st->pre.h9_h) st->pre.h9_swept_dir = -1;
        }
        if (st->pre.h8_ok) {
            if (l < st->pre.h8_l) { st->pre.h8_swept_dir = 1; st->pre.sweep89_dir = 1; }
            else if (h > st->pre.h8_h) { st->pre.h8_swept_dir = -1; st->pre.sweep89_dir = -1; }
        }
        if (st->pre.h9_ok) {
            if (l < st->pre.h9_l) st->pre.sweep89_dir = 1;
            else if (h > st->pre.h9_h) st->pre.sweep89_dir = -1;
        }
    }

    if (v.bar_index() < 30 || v.in_position) return {};

    switch (st->kind) {
    case P2_031_ORB_QUANT: {
        if (!v.or_formed(15) || em < 585 || em >= 900) return {};
        if (v.sess_entries >= 2) return {};
        double orh = v.or_high(15), orl = v.or_low(15), or_rng = orh - orl;
        double min_or = p_at(p, 0, 1.5), max_or = p_at(p, 1, 8.0), rvol_min = p_at(p, 2, 1.0);
        if (or_rng < atr * min_or || or_rng > atr * max_or || v.rel_vol() < rvol_min) return {};
        double sl = std::max(atr * 1.2, or_rng * 0.5), rr = p_at(p, 3, 2.0);
        if (c > orh && static_cast<double>(v[-1].c) <= orh)
            return make_sig(1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 90, 5, inst);
        if (c < orl && static_cast<double>(v[-1].c) >= orl)
            return make_sig(-1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P2_032_S1_PD_DOUBLE: return eval_032_sub(1, *st, v, atr);
    case P2_032_S2_PM_DOUBLE: return eval_032_sub(2, *st, v, atr);
    case P2_032_S3_INTRADAY_DOUBLE: return eval_032_sub(3, *st, v, atr);
    case P2_032_S4_VWAP_REV: return eval_032_sub(4, *st, v, atr);
    case P2_032_S5_EMA200_REV: return eval_032_sub(5, *st, v, atr);
    case P2_032_COMBINED: {
        for (int s = 1; s <= 5; ++s) {
            auto sig = eval_032_sub(s, *st, v, atr);
            if (sig.direction != 0) return sig;
        }
        return {};
    }

    case P2_033_EMA_VWAP_CROSS: {
        if (!st->tf3.just_closed) return {};
        const auto& b = st->tf3.bar(0);
        double vw = v.session_vwap(-1), e9 = v.ema9(-1), e20 = v.ema20(-1);
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.5);
        if (b.o < vw && b.c > vw && e9 > e20)
            return make_sig(1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 60, 3, inst);
        if (b.o > vw && b.c < vw && e9 < e20)
            return make_sig(-1, 0, 0.0, sl, sl * rr, sl * 0.8, sl * 0.4, 0.0, 0.0, 60, 3, inst);
        return {};
    }

    case P2_034_SHORT_NQ_GAMMA_CEIL: {
        // 2-of-3 cross-asset ceiling proxy (PDH/ES/YM or VWAP+1.5std fallback) + buyer exhaustion
        if (em < 570 || em >= 900) return {};
        int hits = 0;
        if (v.has_prior_day() && h >= v.pdh() - atr * 0.5) hits++;
        bool has_x = v.has_ref_es() || v.has_ref_ym();
        if (v.has_ref_es() && static_cast<double>(v.ref_es_bar(0).h) >= static_cast<double>(v.ref_es_bar(-1).h)) hits++;
        if (v.has_ref_ym() && static_cast<double>(v.ref_ym_bar(0).h) >= static_cast<double>(v.ref_ym_bar(-1).h)) hits++;
        if (!has_x) {
            double vw = v.session_vwap(), vws = std::max(atr, v.session_vwap_std());
            if (h >= vw + 1.5 * vws) hits++;
        }
        if (hits >= 2 && c < o && st->ind.cvd < st->ind.prev_cvd) {
            double sl = atr * p_at(p, 0, 1.5);
            return make_sig(-1, 0, 0.0, sl, sl * p_at(p, 1, 2.0), 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        }
        return {};
    }

    case P2_035_SHORT_ABP_FVG: {
        // Bearish FVG + EVP close below prior bar open
        FvgZone z;
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.5);
        if (h > static_cast<double>(v[-1].h) && c < static_cast<double>(v[-1].o) && st->fvg15.find_tap(l, h, -1, false, 20, z)) {
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_036_OR_FAKEOUT_FLOW: {
        if (!v.or_formed(15) || em < 585 || em >= 840) return {};
        double orh = v.or_high(15), orl = v.or_low(15);
        if (h > orh && c < orh) st->or_fakeout_dir = -1;
        else if (l < orl && c > orl) st->or_fakeout_dir = 1;
        if (st->or_fakeout_dir == -1 && h >= orh - atr * 0.5 && c < o && st->ind.cvd < st->ind.prev_cvd) {
            st->or_retest_cnt++;
            if (st->or_retest_cnt >= 2) {
                st->or_fakeout_dir = 0; st->or_retest_cnt = 0;
                double sl = atr * p_at(p, 0, 1.5);
                return make_sig(-1, 0, 0.0, sl, sl * p_at(p, 1, 2.0), 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            }
        }
        if (st->or_fakeout_dir == 1 && l <= orl + atr * 0.5 && c > o && st->ind.cvd > st->ind.prev_cvd) {
            st->or_retest_cnt++;
            if (st->or_retest_cnt >= 2) {
                st->or_fakeout_dir = 0; st->or_retest_cnt = 0;
                double sl = atr * p_at(p, 0, 1.5);
                return make_sig(1, 0, 0.0, sl, sl * p_at(p, 1, 2.0), 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            }
        }
        return {};
    }

    case P2_037_30M_AMD_SMT_CISD: {
        // 9am candle H/L sweep + cross-asset SMT + 3m CISD in NY_AM
        if (!st->pre.h9_ok || em < 570 || em >= 690 || v.sess_entries >= 2) return {};
        if (!st->tf3.just_closed) return {};
        const auto& b = st->tf3.bar(0);
        bool smt_bull = v.has_ref_es() && (l <= st->pre.h9_l) && (static_cast<double>(v.ref_es_bar(0).l) > static_cast<double>(v.ref_es_bar(-5).l));
        bool smt_bear = v.has_ref_es() && (h >= st->pre.h9_h) && (static_cast<double>(v.ref_es_bar(0).h) < static_cast<double>(v.ref_es_bar(-5).h));
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.5);
        if ((st->pre.h9_swept_dir == 1 || smt_bull) && b.c > b.o) {
            st->pre.h9_swept_dir = 0;
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if ((st->pre.h9_swept_dir == -1 || smt_bear) && b.c < b.o) {
            st->pre.h9_swept_dir = 0;
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_038_VP_OVERLAP_SD: {
        // Prior day + week VP POC/VAH/VAL overlap + 2-SD VWAP overextension
        if (!st->vp_prev_day.valid || !st->vp_week.valid) return {};
        double vw = v.session_vwap(), vws = std::max(atr, v.session_vwap_std());
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        bool near_pd = std::abs(c - st->vp_prev_day.poc) < atr || std::abs(c - st->vp_prev_day.vah) < atr || std::abs(c - st->vp_prev_day.val) < atr;
        bool near_wk = std::abs(c - st->vp_week.poc) < atr * 1.5;
        if (near_pd && near_wk) {
            if (c > vw + 1.5 * vws && c < o) return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
            if (c < vw - 1.5 * vws && c > o) return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        }
        return {};
    }

    case P2_039_8AM_9AM_SWEEP_DISP: {
        if (!st->pre.h8_ok || !st->pre.h9_ok || em < 570 || em >= 690 || v.sess_entries >= 2) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (st->pre.sweep89_dir == 1 && c > o && (c - o) >= atr * 0.7) {
            st->pre.sweep89_dir = 0;
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (st->pre.sweep89_dir == -1 && c < o && (o - c) >= atr * 0.7) {
            st->pre.sweep89_dir = 0;
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_040_REJ_BLOCK_50_IFVG: {
        // Rejection wick 50% midpoint + 1m IFVG
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->fvg1.find_tap(l, h, 1, true, 12, z) && st->sw1.sweep_lo_now) {
            double sl = std::max(atr, c - l + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        if (st->fvg1.find_tap(l, h, -1, true, 12, z) && st->sw1.sweep_hi_now) {
            double sl = std::max(atr, h - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        }
        return {};
    }

    case P2_041_4STEP_FILTER_CISD: {
        // Sweep + 15m FVG + 1m IFVG + CISD
        FvgZone z1, z15;
        double rr = p_at(p, 0, 2.0);
        if (st->fvg15.find_tap(l, h, 1, false, 20, z15) && st->fvg1.find_tap(l, h, 1, true, 15, z1) && c > o) {
            double sl = std::max(atr, c - z1.lo + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        }
        if (st->fvg15.find_tap(l, h, -1, false, 20, z15) && st->fvg1.find_tap(l, h, -1, true, 15, z1) && c < o) {
            double sl = std::max(atr, z1.hi - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        }
        return {};
    }

    case P2_042_MTF_HS_BOS_EMA: {
        // 30m BOS below/above EMA50
        if (!st->tf30.just_closed) return {};
        double sl = atr * p_at(p, 0, 2.0), rr = p_at(p, 1, 2.0);
        if (st->sw15.bos_up_now && c > v.ema50()) return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        if (st->sw15.bos_dn_now && c < v.ema50()) return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P2_043_ONE_CANDLE_6R:
    case P2_044_REJ_BLOCK_96WR: {
        // 4H PD array -> 15m rejection block 50% limit, fixed 10pt SL, 6R target
        if (!st->tf15.just_closed) return {};
        const auto& b = st->tf15.bar(0);
        double rng = std::max(inst.tick_size, b.h - b.l);
        double sl_pts = p_at(p, 0, 10.0) * sym_pt_scale(inst);
        double rr = p_at(p, 1, 6.0);
        FvgZone z;
        if (st->fvg240.find_tap(b.l, b.h, 1, false, 16, z) && (std::min(b.o, b.c) - b.l) / rng >= 0.30) {
            double entry = 0.5 * (b.l + std::min(b.o, b.c));
            return make_sig(1, 2, entry, sl_pts, sl_pts * rr, 0.0, 0.0, sl_pts * 1.5, inst.tick_size, 90, 4, inst);
        }
        if (st->fvg240.find_tap(b.l, b.h, -1, false, 16, z) && (b.h - std::max(b.o, b.c)) / rng >= 0.30) {
            double entry = 0.5 * (b.h + std::max(b.o, b.c));
            return make_sig(-1, 2, entry, sl_pts, sl_pts * rr, 0.0, 0.0, sl_pts * 1.5, inst.tick_size, 90, 4, inst);
        }
        return {};
    }

    case P2_045_8AM_SWEEP_3M_IFVG: {
        if (!st->pre.h8_ok || em < 570 || em >= 690 || v.sess_entries >= 2) return {};
        FvgZone z;
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (st->pre.h8_swept_dir == 1 && st->fvg3.find_tap(l, h, 1, true, 16, z)) {
            st->pre.h8_swept_dir = 0;
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (st->pre.h8_swept_dir == -1 && st->fvg3.find_tap(l, h, -1, true, 16, z)) {
            st->pre.h8_swept_dir = 0;
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_046_OMAR_NOWICK: {
        // Bullish candle with no bottom wick / Bearish candle with no top wick in trend -> 1:1 R:R (RTH gated)
        if (em < 570 || em >= 960 || v.sess_entries >= 3 || !st->tf15.just_closed) return {};
        const auto& b = st->tf15.bar(0);
        double rng = std::max(inst.tick_size, b.h - b.l);
        double rr = p_at(p, 0, 1.0);
        if (b.c > b.o && (b.o - b.l) <= 0.10 * rng && v.ema9(-1) > v.ema20(-1)) {
            double sl = std::max(atr, b.c - b.l + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
        }
        if (b.c < b.o && (b.h - b.o) <= 0.10 * rng && v.ema9(-1) < v.ema20(-1)) {
            double sl = std::max(atr, b.h - b.c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
        }
        return {};
    }

    case P2_047_15M_SWEEP_2M_FVG: {
        if (!st->tf2.just_closed) return {};
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->sw15.has_sl && l < st->sw15.last_sl && st->fvg1.find_tap(l, h, 1, false, 15, z)) {
            double sl = std::max(atr, c - l + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (st->sw15.has_sh && h > st->sw15.last_sh && st->fvg1.find_tap(l, h, -1, false, 15, z)) {
            double sl = std::max(atr, h - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_048_CLOSE_SWEEP_FVG: {
        // Real sweep requires candle CLOSE beyond swing, then shift + FVG retrace
        FvgZone z;
        double rr = p_at(p, 0, 2.0);
        if (st->sw15.bos_up_now || (st->fvg1.find_tap(l, h, 1, false, 12, z) && c > o)) {
            double sl = std::max(atr, c - l + 2.0 * inst.tick_size);
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        }
        if (st->sw15.bos_dn_now || (st->fvg1.find_tap(l, h, -1, false, 12, z) && c < o)) {
            double sl = std::max(atr, h - c + 2.0 * inst.tick_size);
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 4, inst);
        }
        return {};
    }

    case P2_049_ICT_OTE_79: {
        if (!st->sw1.has_sh || !st->sw1.has_sl) return {};
        double rng = st->sw1.last_sh - st->sw1.last_sl;
        if (rng < atr * 2.0) return {};
        double rr = p_at(p, 0, 2.0);
        if (st->sw1.trend_dir == 1) {
            double ote = st->sw1.last_sh - 0.705 * rng;
            double sl_px = st->sw1.last_sh - 0.79 * rng;
            if (l <= ote && c > sl_px && c > o) {
                double sl = std::max(atr, c - sl_px + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            }
        } else if (st->sw1.trend_dir == -1) {
            double ote = st->sw1.last_sl + 0.705 * rng;
            double sl_px = st->sw1.last_sl + 0.79 * rng;
            if (h >= ote && c < sl_px && c < o) {
                double sl = std::max(atr, sl_px - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            }
        }
        return {};
    }

    case P2_050_EVP_TYPE1_FRONTSIDE:
    case P2_050_EVP_TYPE2_CONFIRM:
    case P2_050_EVP_COMBINED: {
        // EVP candle on 15m (RTH gated, max 3/session)
        if (em < 570 || em >= 960 || v.sess_entries >= 3) return {};
        double rr = p_at(p, 0, 2.0);
        if (st->tf15.just_closed && st->tf15.size() >= 2) {
            const auto& b0 = st->tf15.bar(0);
            const auto& b1 = st->tf15.bar(1);
            bool bull_evp = (b0.l < b1.l && b0.c > b1.o);
            bool bear_evp = (b0.h > b1.h && b0.c < b1.o);
            if (bull_evp) {
                if (st->kind == P2_050_EVP_TYPE1_FRONTSIDE || st->kind == P2_050_EVP_COMBINED) {
                    double entry = 0.5 * (b0.o + b0.c);
                    double sl = std::max(atr, entry - b0.l + 2.0 * inst.tick_size);
                    return make_sig(1, 2, entry, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
                }
                st->pending_evp_dir = 1; st->pending_evp_sl_px = b0.l; st->pending_evp_age = 0;
            } else if (bear_evp) {
                if (st->kind == P2_050_EVP_TYPE1_FRONTSIDE || st->kind == P2_050_EVP_COMBINED) {
                    double entry = 0.5 * (b0.o + b0.c);
                    double sl = std::max(atr, b0.h - entry + 2.0 * inst.tick_size);
                    return make_sig(-1, 2, entry, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
                }
                st->pending_evp_dir = -1; st->pending_evp_sl_px = b0.h; st->pending_evp_age = 0;
            }
        }
        if ((st->kind == P2_050_EVP_TYPE2_CONFIRM || st->kind == P2_050_EVP_COMBINED) && st->pending_evp_dir != 0) {
            FvgZone z;
            if (st->pending_evp_dir == 1 && (st->sw1.bos_up_now || st->fvg1.find_tap(l, h, 1, true, 10, z))) {
                st->pending_evp_dir = 0;
                double sl = std::max(atr, c - st->pending_evp_sl_px + 2.0 * inst.tick_size);
                return make_sig(1, 0, 0.0, sl, sl * rr * 1.25, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
            }
            if (st->pending_evp_dir == -1 && (st->sw1.bos_dn_now || st->fvg1.find_tap(l, h, -1, true, 10, z))) {
                st->pending_evp_dir = 0;
                double sl = std::max(atr, st->pending_evp_sl_px - c + 2.0 * inst.tick_size);
                return make_sig(-1, 0, 0.0, sl, sl * rr * 1.25, 0.0, 0.0, 0.0, 0.0, 60, 3, inst);
            }
        }
        return {};
    }

    case P2_051_4H_PRED_STOPRUN: {
        // 1H stop run + close above/below 3 opposing candles
        if (v.bar_index() < 5) return {};
        double rr = p_at(p, 0, 2.0), sl = atr * p_at(p, 1, 1.5);
        bool three_dn = (v[-1].c < v[-1].o && v[-2].c < v[-2].o && v[-3].c < v[-3].o);
        bool three_up = (v[-1].c > v[-1].o && v[-2].c > v[-2].o && v[-3].c > v[-3].o);
        if (three_dn && c > static_cast<double>(v[-3].o))
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (three_up && c < static_cast<double>(v[-3].o))
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P2_052_830_NEWS_CONT_FADE: {
        // 08:30 ET news shock scanned from causal past bars -> continuation or fade executed at NY_AM open (09:30-11:00)
        if (em < 570 || em >= 660 || st->pre.news_dir == 0 || v.sess_entries >= 2) return {};
        double sl = atr * p_at(p, 0, 2.0);
        double dist = std::abs(c - st->pre.pre_news_px);
        if (!st->news_dir_taken) {
            st->news_dir_taken = true;
            return make_sig(st->pre.news_dir, 0, 0.0, sl, sl * 1.5, 0.0, 0.0, 0.0, 0.0, 20, 2, inst);
        }
        if (dist >= atr * 2.0) {
            int8_t fdir = -st->pre.news_dir;
            st->pre.news_dir = 0;
            return make_sig(fdir, 0, 0.0, sl, std::max(sl * 1.2, dist * 0.7), 0.0, 0.0, 0.0, 0.0, 45, 5, inst);
        }
        return {};
    }

    case P2_053_ASIA_LON_NY_INV: {
        // NY open sweeps Asia/London H/L + 1m IFVG
        if (em < 570 || em >= 720) return {};
        FvgZone z;
        double rr = p_at(p, 0, 2.0), sl = atr * p_at(p, 1, 1.5);
        if ((st->sess.asia_ok && l < st->sess.asia_l) || (st->sess.lon_ok && l < st->sess.lon_l)) {
            if (st->fvg1.find_tap(l, h, 1, true, 15, z))
                return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if ((st->sess.asia_ok && h > st->sess.asia_h) || (st->sess.lon_ok && h > st->sess.lon_h)) {
            if (st->fvg1.find_tap(l, h, -1, true, 15, z))
                return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_054_OVN_VP_BREAKOUT: {
        // Overnight VP (16:30-09:30) Value Area breakout
        if (em < 570 || em >= 840 || !st->vp_ovn.valid) return {};
        double sl = atr * p_at(p, 0, 1.8), rr = p_at(p, 1, 2.0);
        if (c > st->vp_ovn.vah && static_cast<double>(v[-1].c) <= st->vp_ovn.vah)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        if (c < st->vp_ovn.val && static_cast<double>(v[-1].c) >= st->vp_ovn.val)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P2_055_ASIA_HUNT_NY_LATE: {
        // Asia (18:00-23:59) sweeps prior NY final hours (14:00-16:00) H/L and reverses
        if (em < 1080 || !st->sess.has_prev_ny_late) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (h > st->sess.prev_ny_late_h && c < st->sess.prev_ny_late_h && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        if (l < st->sess.prev_ny_late_l && c > st->sess.prev_ny_late_l && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 5, inst);
        return {};
    }

    case P2_056_OVN_ACCUM_1H_OB: {
        if (em < 570 || em >= 720 || !st->vp_ovn.valid) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (l <= st->vp_ovn.val && c > st->vp_ovn.val && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (h >= st->vp_ovn.vah && c < st->vp_ovn.vah && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P2_057_PD_PW_HVN_REACT: {
        if (!st->vp_prev_day.valid || !st->vp_week.valid) return {};
        double sl = atr * p_at(p, 0, 1.5), rr = p_at(p, 1, 2.0);
        if (st->vp_prev_day.is_hvn(c, 1.35) && st->vp_week.is_hvn(c, 1.25)) {
            if (c > st->vp_prev_day.poc && c < o) return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
            if (c < st->vp_prev_day.poc && c > o) return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }

    case P2_058_LON_ASIA_SWEEP_IFVG: {
        if (em < 120 || em >= 510 || !st->sess.asia_ok) return {};
        FvgZone z;
        double sl = atr * p_at(p, 0, 1.5);
        if (l < st->sess.asia_l && st->fvg1.find_tap(l, h, 1, true, 15, z))
            return make_sig(1, 0, 0.0, sl, std::max(sl * 2.0, st->sess.asia_h - c), 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        if (h > st->sess.asia_h && st->fvg1.find_tap(l, h, -1, true, 15, z))
            return make_sig(-1, 0, 0.0, sl, std::max(sl * 2.0, c - st->sess.asia_l), 0.0, 0.0, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P2_059_LVN_LOD_RECLAIM: {
        // Standalone #059: Composite/Day LVN pass-through LOD/HOD reclaim
        if (!st->vp_day.valid) return {};
        double lvn_r = p_at(p, 0, 0.85), rr = p_at(p, 1, 2.5);
        if (st->vp_day.is_lvn(c, lvn_r) && st->sw1.sweep_lo_now && c > o)
            return make_sig(1, 0, 0.0, atr * 1.5, atr * 1.5 * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        if (st->vp_day.is_lvn(c, lvn_r) && st->sw1.sweep_hi_now && c < o)
            return make_sig(-1, 0, 0.0, atr * 1.5, atr * 1.5 * rr, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        return {};
    }

    case P2_060_LVN_WICK_AGGR: {
        // Standalone #060: LVN pass-through + wick/volume aggression continuation
        double lvn_r = p_at(p, 0, 0.85), wick_m = p_at(p, 1, 0.25), rvol_m = p_at(p, 2, 1.1);
        double sl = atr * 1.5, rr = p_at(p, 3, 2.0);
        if (LvnRegimeFilter::allows(st->vp_day, v, 1, lvn_r, wick_m, rvol_m) && c > o)
            return make_sig(1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        if (LvnRegimeFilter::allows(st->vp_day, v, -1, lvn_r, wick_m, rvol_m) && c < o)
            return make_sig(-1, 0, 0.0, sl, sl * rr, 0.0, 0.0, 0.0, 0.0, 45, 3, inst);
        return {};
    }

    case P2_059_060_FILTER_ORB: {
        // Anchor Strategy 1 (15m ORB) gated by #059/#060 LVN + Wick Aggression Filter
        if (!v.or_formed(15) || em < 585 || em >= 840) return {};
        double orh = v.or_high(15), orl = v.or_low(15);
        double lvn_r = p_at(p, 0, 0.85), wick_m = p_at(p, 1, 0.20), rvol_m = p_at(p, 2, 0.90);
        double sl = std::max(atr * 1.2, 0.5 * (orh - orl));
        if (c > orh && static_cast<double>(v[-1].c) <= orh && LvnRegimeFilter::allows(st->vp_day, v, 1, lvn_r, wick_m, rvol_m))
            return make_sig(1, 0, 0.0, sl, sl * 2.5, sl * 0.8, sl * 0.4, 0.0, 0.0, 90, 5, inst);
        if (c < orl && static_cast<double>(v[-1].c) >= orl && LvnRegimeFilter::allows(st->vp_day, v, -1, lvn_r, wick_m, rvol_m))
            return make_sig(-1, 0, 0.0, sl, sl * 2.5, sl * 0.8, sl * 0.4, 0.0, 0.0, 90, 5, inst);
        return {};
    }

    case P2_059_060_FILTER_EMA_VWAP: {
        // Anchor Strategy 2 (EMA9/20 + VWAP) gated by #059/#060 LVN + Wick Aggression Filter
        double lvn_r = p_at(p, 0, 0.85), wick_m = p_at(p, 1, 0.20), rvol_m = p_at(p, 2, 0.90);
        double sl = atr * 1.5;
        if (v.ema9() > v.ema20() && c > v.session_vwap() && static_cast<double>(v[-1].c) <= v.ema9(-1) && c > v.ema9()) {
            if (LvnRegimeFilter::allows(st->vp_day, v, 1, lvn_r, wick_m, rvol_m))
                return make_sig(1, 0, 0.0, sl, sl * 2.2, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        if (v.ema9() < v.ema20() && c < v.session_vwap() && static_cast<double>(v[-1].c) >= v.ema9(-1) && c < v.ema9()) {
            if (LvnRegimeFilter::allows(st->vp_day, v, -1, lvn_r, wick_m, rvol_m))
                return make_sig(-1, 0, 0.0, sl, sl * 2.2, 0.0, 0.0, 0.0, 0.0, 60, 4, inst);
        }
        return {};
    }
    }
    return {};
}

template <int K>
inline void add_p2(std::vector<s546::RegisteredStrategy>& out,
                   const std::string& uid, const std::string& name,
                   const std::string& src, int32_t smask, bool xasset,
                   std::vector<u11::ParamSpec> params) {
    s546::RegisteredStrategy r;
    r.uid = uid;
    r.display_name = name;
    r.super_family = "IG91_Part2_031_060";
    r.source_note = src;
    r.fidelity = xasset ? "CROSS_ASSET_CAUSAL" : "CAUSAL_REEL_SPEC";
    r.needs_cross_asset = xasset;
    r.session_mask = smask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<P2State>(uid, p2_on_bar, p2_init<K>);
    out.push_back(std::move(r));
}

inline void register_ig91_part2(std::vector<s546::RegisteredStrategy>& out) {
    add_p2<P2_031_ORB_QUANT>(out, "ig91_031_orb_supercharged_quant", "Reel 031: Supercharged ORB (ATR/RVOL/Range)", "031_DeNOJGuBMp8", 0xF, false, {p_lin("min_or_atr", 1.5, 0.5, 3.0), p_lin("max_or_atr", 8.0, 4.0, 14.0), p_lin("rvol_min", 1.0, 0.6, 2.0), p_lin("rr", 2.0, 1.0, 4.0)});
    add_p2<P2_032_S1_PD_DOUBLE>(out, "ig91_032_s1_pdh_pdl_double", "Reel 032 Sub-1: PDH/PDL Double Top/Bottom", "032_Dd0y9HUpOYr", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_032_S2_PM_DOUBLE>(out, "ig91_032_s2_pmh_pml_double", "Reel 032 Sub-2: Pre-Market H/L Double Top/Bottom", "032_Dd0y9HUpOYr", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_032_S3_INTRADAY_DOUBLE>(out, "ig91_032_s3_intraday_double", "Reel 032 Sub-3: Intraday H/L Double Top/Bottom", "032_Dd0y9HUpOYr", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_032_S4_VWAP_REV>(out, "ig91_032_s4_vwap_reversal", "Reel 032 Sub-4: VWAP Reversal", "032_Dd0y9HUpOYr", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_032_S5_EMA200_REV>(out, "ig91_032_s5_ema200_reversal", "Reel 032 Sub-5: 200-EMA Reversal", "032_Dd0y9HUpOYr", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_032_COMBINED>(out, "ig91_032_combined_5strats", "Reel 032 Combined: All 5 Sub-Strategies", "032_Dd0y9HUpOYr", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_033_EMA_VWAP_CROSS>(out, "ig91_033_ema_vwap_cross_3m", "Reel 033: 3m EMA + VWAP Cross", "033_DeWsJBsA6cv", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p2<P2_034_SHORT_NQ_GAMMA_CEIL>(out, "ig91_034_short_nq_gamma_ceil", "Reel 034: Short NQ 2-of-3 Ceiling (OHLCV Proxy)", "034_DedXH5Uo4Tt", 0xF, true, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_035_SHORT_ABP_FVG>(out, "ig91_035_short_abp_fvg", "Reel 035: Bearish FVG + EVP Short", "035_Deew3R6gBeL", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p2<P2_036_OR_FAKEOUT_FLOW>(out, "ig91_036_or_fakeout_flow", "Reel 036: 15m OR Fakeout + CVD Exhaustion", "036_DefGsR2RkOy", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_037_30M_AMD_SMT_CISD>(out, "ig91_037_30m_amd_smt_cisd", "Reel 037: 9AM Sweep + SMT + 3m CISD", "037_DefdpOLAsEa", 0xF, true, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p2<P2_038_VP_OVERLAP_SD>(out, "ig91_038_vp_overlap_sd", "Reel 038: Multi-TF VP Overlap + SD Overextension", "038_DeiBnQighr8", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_039_8AM_9AM_SWEEP_DISP>(out, "ig91_039_8am_9am_sweep_disp", "Reel 039: 8AM/9AM Candle Sweep + Displacement", "039_DeR0FBUsljG", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_040_REJ_BLOCK_50_IFVG>(out, "ig91_040_rej_block_50_ifvg", "Reel 040: Rejection Block 50% + IFVG", "040_DehQ042tRD5", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_041_4STEP_FILTER_CISD>(out, "ig91_041_4step_filter_cisd", "Reel 041: 4-Step Filter (Sweep+HTF FVG+IFVG+CISD)", "041_Dei4ofxKOkL", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_042_MTF_HS_BOS_EMA>(out, "ig91_042_mtf_hs_bos_ema", "Reel 042: Multi-TF H&S BOS + EMA50", "042_DejXrbqtkkD", 0xF, false, {p_lin("sl_atr", 2.0, 1.0, 3.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_043_ONE_CANDLE_6R>(out, "ig91_043_one_candle_6r", "Reel 043: One Candle Entry Model (6R)", "043_DekdH7XB330", 0xF, false, {p_lin("sl_pts", 10.0, 5.0, 25.0), p_lin("rr", 6.0, 3.0, 8.0)});
    add_p2<P2_044_REJ_BLOCK_96WR>(out, "ig91_044_rej_block_96wr", "Reel 044: Rejection Block 50% (6R)", "044_DcRGOOfhyUu", 0xF, false, {p_lin("sl_pts", 10.0, 5.0, 25.0), p_lin("rr", 6.0, 3.0, 8.0)});
    add_p2<P2_045_8AM_SWEEP_3M_IFVG>(out, "ig91_045_8am_sweep_3m_ifvg", "Reel 045: 8AM Sweep + 3m IFVG", "045_DekUkYexZaK", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_046_OMAR_NOWICK>(out, "ig91_046_omar_nowick_15m", "Reel 046: Omar No-Wick 15m Trend", "046_DecxaiGtJnt", 0xF, false, {p_lin("rr", 1.0, 0.8, 2.5)});
    add_p2<P2_047_15M_SWEEP_2M_FVG>(out, "ig91_047_15m_sweep_2m_fvg", "Reel 047: 15m Swing Sweep + 2m FVG", "047_DekDXtWSi2c", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_048_CLOSE_SWEEP_FVG>(out, "ig91_048_close_sweep_fvg", "Reel 048: Real Close Sweep + FVG", "048_Dem_Z2cNrfh", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_049_ICT_OTE_79>(out, "ig91_049_ict_ote_79", "Reel 049: ICT OTE 70.5% / 0.79 Stop", "049_Dedn4uNNuyK", 0xF, false, {p_lin("rr", 2.0, 1.0, 4.0)});
    add_p2<P2_050_EVP_TYPE1_FRONTSIDE>(out, "ig91_050_evp_type1_frontside", "Reel 050 Type 1: EVP Front-Side Retrace", "050_DecWxD0g6-r", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_050_EVP_TYPE2_CONFIRM>(out, "ig91_050_evp_type2_confirm", "Reel 050 Type 2: EVP LTF Confirmation", "050_DecWxD0g6-r", 0xF, false, {p_lin("rr", 2.0, 1.0, 4.0)});
    add_p2<P2_050_EVP_COMBINED>(out, "ig91_050_evp_combined", "Reel 050 Combined: EVP Type 1 + Type 2", "050_DecWxD0g6-r", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_051_4H_PRED_STOPRUN>(out, "ig91_051_4h_pred_stoprun", "Reel 051: 3-Candle Stop-Run Reclaim", "051_Dej5a4OoIcF", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p2<P2_052_830_NEWS_CONT_FADE>(out, "ig91_052_830_news_cont_fade", "Reel 052: 08:30 News Continuation + Fade", "052_DeqM-LllOsu", 0xF, false, {p_lin("sl_atr", 2.0, 1.0, 3.5)});
    add_p2<P2_053_ASIA_LON_NY_INV>(out, "ig91_053_asia_lon_ny_inv", "Reel 053: Asia/London Sweep + NY IFVG", "053_Dejm0vrMHjc", 0xF, false, {p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p2<P2_054_OVN_VP_BREAKOUT>(out, "ig91_054_ovn_vp_breakout", "Reel 054: Overnight VP Value Area Breakout", "054_DZrDGx9tzMq", 0xF, false, {p_lin("sl_atr", 1.8, 1.0, 3.0), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_055_ASIA_HUNT_NY_LATE>(out, "ig91_055_asia_hunt_ny_late", "Reel 055: Asia Hunts Prior NY Late H/L", "055_DbSpUsCO7Jn", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_056_OVN_ACCUM_1H_OB>(out, "ig91_056_ovn_accum_1h_ob", "Reel 056: Overnight Accum + NY AM Reversal", "056_DesUk2xhDGH", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_057_PD_PW_HVN_REACT>(out, "ig91_057_pd_pw_hvn_react", "Reel 057: PD + PW High Volume Node Reaction", "057_DekdURtkfco", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_058_LON_ASIA_SWEEP_IFVG>(out, "ig91_058_lon_asia_sweep_ifvg", "Reel 058: London Asia Sweep + IFVG", "058_Deu1yMPAWz5", 0xF, false, {p_lin("sl_atr", 1.5, 0.8, 2.5)});
    add_p2<P2_059_LVN_LOD_RECLAIM>(out, "ig91_059_lvn_lod_reclaim", "Reel 059: LVN Pass-Through LOD/HOD Reclaim", "059_DeuDlUszkL8", 0xF, false, {p_lin("lvn_ratio", 0.85, 0.4, 1.1), p_lin("rr", 2.5, 1.2, 4.0)});
    add_p2<P2_060_LVN_WICK_AGGR>(out, "ig91_060_lvn_wick_aggression", "Reel 060: LVN + Wick Volume Aggression", "060_DeuwNdCzPEs", 0xF, false, {p_lin("lvn_ratio", 0.85, 0.4, 1.1), p_lin("wick_min", 0.25, 0.1, 0.5), p_lin("rvol_min", 1.1, 0.7, 2.0), p_lin("rr", 2.0, 1.0, 3.5)});
    add_p2<P2_059_060_FILTER_ORB>(out, "ig91_059_060_filter_on_orb", "Reels 059+060 Filter Overlay on 15m ORB Anchor", "059_060_Filter", 0xF, false, {p_lin("lvn_ratio", 0.85, 0.4, 1.1), p_lin("wick_min", 0.20, 0.05, 0.45), p_lin("rvol_min", 0.90, 0.5, 1.8)});
    add_p2<P2_059_060_FILTER_EMA_VWAP>(out, "ig91_059_060_filter_on_ema_vwap", "Reels 059+060 Filter Overlay on EMA+VWAP Anchor", "059_060_Filter", 0xF, false, {p_lin("lvn_ratio", 0.85, 0.4, 1.1), p_lin("wick_min", 0.20, 0.05, 0.45), p_lin("rvol_min", 0.90, 0.5, 1.8)});
}

} // namespace ig91
} // namespace s546
