#pragma once
// ============================================================================
// BATCH 3 PLUGINS (154 Strategies):
//   - 105 Physics Quant Multiverse (phys_116..120, 141..240)
//   -  49 Instagram Reels Matrix (kernel_reel_1/2/6 + reel46_01..46)
// Strictly CausalBarView (lag <= 0), InstrumentSpec-relative (NQ, ES, YM),
// parameterized in u11::ParamSchema, ASan/UBSan & -Wall -Wextra -Werror clean.
// ============================================================================

#include "strategy_registry_546.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace s546 {
namespace b3_detail {

inline double p_or(const std::vector<double>& p, size_t idx, double def_v) {
    return (idx < p.size()) ? p[idx] : def_v;
}

inline double inst_ratio(const u11::InstrumentSpec& inst) {
    return inst.baseline_index / 4524.25;
}

inline double price_scale(double open_px, const u11::InstrumentSpec& inst) {
    return std::clamp(open_px / std::max(1.0, inst.baseline_index), 0.70, 3.00);
}

// Causal cross-asset reference helpers (fall back cleanly to self when ref not attached)
inline const u11::RawBar& es_ref_bar(const u11::CausalBarView& v, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol == "ES") {
        return v.has_ref_nq() ? v.ref_nq_bar(lag) : v[lag];
    }
    return v.has_ref_es() ? v.ref_es_bar(lag) : v[lag];
}

inline double es_ref_atr14(const u11::CausalBarView& v, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol == "ES") {
        return v.has_ref_nq() ? v.ref_nq_atr14(lag) : v.atr14(lag);
    }
    return v.has_ref_es() ? v.ref_es_atr14(lag) : v.atr14(lag);
}

inline const u11::RawBar& ref1_bar(const u11::CausalBarView& v, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol == "NQ") return v.has_ref_es() ? v.ref_es_bar(lag) : v[lag];
    return v.has_ref_nq() ? v.ref_nq_bar(lag) : v[lag];
}

inline double ref1_atr14(const u11::CausalBarView& v, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol == "NQ") return v.has_ref_es() ? v.ref_es_atr14(lag) : v.atr14(lag);
    return v.has_ref_nq() ? v.ref_nq_atr14(lag) : v.atr14(lag);
}

inline const u11::RawBar& ref2_bar(const u11::CausalBarView& v, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol == "YM") return v.has_ref_es() ? v.ref_es_bar(lag) : v[lag];
    return v.has_ref_ym() ? v.ref_ym_bar(lag) : v[lag];
}

inline double ref2_atr14(const u11::CausalBarView& v, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol == "YM") return v.has_ref_es() ? v.ref_es_atr14(lag) : v.atr14(lag);
    return v.has_ref_ym() ? v.ref_ym_atr14(lag) : v.atr14(lag);
}

// ============================================================================
// FAMILY 1: Physics Batch 6 (phys_116 .. phys_120)
// ============================================================================
struct PhysB6State {
    int strat_id = 116;
    u11::InstrumentSpec inst = u11::get_instrument_spec("NQ");
    double thresh = 1.2;
    double sl_atr_mult = 0.85;
    double tp_atr_mult = 4.2;
    double ratchet_trig_r = 1.5;
    double ratchet_trail_r = 0.75;
    int max_entries = 12;
};

inline void init_b6_common(PhysB6State& st, int sid, const std::vector<double>& pv, const u11::InstrumentSpec* sp,
                           double def_th, double def_sl, double def_tp, int def_me) {
    st.strat_id = sid;
    st.inst = sp ? *sp : u11::get_instrument_spec("NQ");
    st.thresh = p_or(pv, 0, def_th);
    st.sl_atr_mult = p_or(pv, 1, def_sl);
    st.tp_atr_mult = p_or(pv, 2, def_tp);
    st.ratchet_trig_r = p_or(pv, 3, 1.5);
    st.ratchet_trail_r = p_or(pv, 4, 0.75);
    st.max_entries = std::max(1, static_cast<int>(std::round(p_or(pv, 5, static_cast<double>(def_me)))));
}

inline u11::PluginSignal phys_b6_on_bar(const u11::CausalBarView& view, void* state) {
    const auto* st = static_cast<const PhysB6State*>(state);
    u11::PluginSignal sig{};
    if (view.session_id() < 0 || view.in_position || view.sess_entries >= st->max_entries) return sig;
    if (view.bar_index() < 10) return sig;

    const double cur_atr = std::max(st->inst.tick_size, view.atr14(0));
    bool sig_long = false;
    bool sig_short = false;

    if (st->strat_id == 116) {
        double p0 = static_cast<double>(view[0].c);
        double p1 = static_cast<double>(view[-3].c);
        double p2 = static_cast<double>(view[-7].c);
        double d10 = p0 - p1;
        double d21 = p1 - p2;
        if (d10 > st->thresh * cur_atr && d21 > st->thresh * cur_atr) sig_long = true;
        else if (d10 < -st->thresh * cur_atr && d21 < -st->thresh * cur_atr) sig_short = true;
    } else if (st->strat_id == 117) {
        double es_atr = std::max(1e-4, es_ref_atr14(view, st->inst, 0));
        double r_self = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
        double r_es = (static_cast<double>(es_ref_bar(view, st->inst, 0).c) - static_cast<double>(es_ref_bar(view, st->inst, -1).c)) / es_atr;
        if (r_self > st->thresh && r_es > st->thresh) sig_long = true;
        else if (r_self < -st->thresh && r_es < -st->thresh) sig_short = true;
    } else if (st->strat_id == 118) {
        int s1 = (view[0].c > view[-1].c) ? 1 : -1;
        int s2 = (view[-1].c > view[-2].c) ? 1 : -1;
        int s3 = (view[-2].c > view[-3].c) ? 1 : -1;
        double move3 = static_cast<double>(view[0].c) - static_cast<double>(view[-3].c);
        if (s1 == 1 && s2 == 1 && s3 == 1 && move3 > st->thresh * cur_atr) sig_long = true;
        else if (s1 == -1 && s2 == -1 && s3 == -1 && -move3 > st->thresh * cur_atr) sig_short = true;
    } else if (st->strat_id == 119) {
        double vol_speed = static_cast<double>(view[0].v) / (static_cast<double>(view[-4].v) + 1e-4);
        double price_acc = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
        if (vol_speed > st->thresh && price_acc > 1.2) sig_long = true;
        else if (vol_speed > st->thresh && price_acc < -1.2) sig_short = true;
    } else if (st->strat_id == 120) {
        double x1 = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
        double x2 = (static_cast<double>(view[-1].c) - static_cast<double>(view[-2].c)) / cur_atr;
        double v_kernel = 0.7 * x1 + 0.3 * x2 + 0.4 * (x1 * x2);
        if (v_kernel > st->thresh) sig_long = true;
        else if (v_kernel < -st->thresh) sig_short = true;
    }

    if (sig_long || sig_short) {
        const double sl_pts = std::max(st->inst.tick_size * 4.0, st->sl_atr_mult * cur_atr);
        sig.direction = sig_long ? 1 : -1;
        sig.order_type = 0;
        sig.sl_pts = sl_pts;
        sig.tp_pts = std::max(0.0, st->tp_atr_mult * cur_atr);
        sig.ratchet_trig_pts = (st->ratchet_trig_r > 0.0) ? (st->ratchet_trig_r * sl_pts) : 0.0;
        sig.ratchet_trail_pts = (st->ratchet_trail_r > 0.0) ? (st->ratchet_trail_r * sl_pts) : 0.0;
        sig.max_hold_bars = 45;
        sig.cooldown_bars = 1;
    }
    return sig;
}

// ============================================================================
// FAMILY 2: Physics Batch 7 & 8 (phys_141 .. phys_180, ORB v5 Filter Matrix)
// ============================================================================
struct PhysOrbV5State {
    int strat_id = 141;
    u11::InstrumentSpec inst = u11::get_instrument_spec("NQ");
    int or_mins = 30;
    int dir_filter = 0; // 0=both, 1=long_only, -1=short_only
    int min_reentry = 1;
    int max_reentry = 12;
    double base_sl_pts = 36.0;
    double rr_tp = 0.0;
    double ratchet_trig_r = 0.50;
    double ratchet_trail_r = 0.25;

    // Causal per-session OR + virtual reentry tracker (supports 3m/5m as well as 15m/30m/45m/60m)
    int32_t cur_day = -1;
    int8_t cur_sess = -1;
    double sess_open_px = 0.0;
    double sess_atr_1h = 0.0;
    double custom_or_h = -1e18;
    double custom_or_l = 1e18;
    bool custom_or_formed = false;

    int virt_entries = 0;
    bool virt_in_pos = false;
    int8_t virt_side = 0;
    double virt_entry_px = 0.0;
    double virt_sl_px = 0.0;
    double virt_peak_mfe = 0.0;
};

inline void init_orbv5_common(PhysOrbV5State& st, int sid, const std::vector<double>& pv, const u11::InstrumentSpec* sp,
                              int def_or_mins, int def_dir, int def_min_r, int def_max_r) {
    st.strat_id = sid;
    st.inst = sp ? *sp : u11::get_instrument_spec("NQ");
    st.or_mins = std::max(3, static_cast<int>(std::round(p_or(pv, 0, static_cast<double>(def_or_mins)))));
    st.base_sl_pts = p_or(pv, 1, 36.0);
    st.rr_tp = p_or(pv, 2, 0.0);
    st.ratchet_trig_r = p_or(pv, 3, 0.50);
    st.ratchet_trail_r = p_or(pv, 4, 0.25);
    st.min_reentry = std::max(1, static_cast<int>(std::round(p_or(pv, 5, static_cast<double>(def_min_r)))));
    st.max_reentry = std::max(st.min_reentry, static_cast<int>(std::round(p_or(pv, 6, static_cast<double>(def_max_r)))));
    st.dir_filter = def_dir;
}

inline u11::PluginSignal phys_orbv5_on_bar(const u11::CausalBarView& view, void* state) {
    auto* st = static_cast<PhysOrbV5State*>(state);
    u11::PluginSignal sig{};

    const int32_t d = view.cme_day_idx();
    const int8_t s_id = view.session_id();
    const auto& b0 = view[0];

    if (d != st->cur_day || s_id != st->cur_sess || view.is_new_sess()) {
        st->cur_day = d;
        st->cur_sess = s_id;
        st->sess_open_px = static_cast<double>(b0.o);
        st->sess_atr_1h = (view.bar_index() > 0) ? view.atr60(-1) : view.atr60(0);
        st->custom_or_h = -1e18;
        st->custom_or_l = 1e18;
        st->custom_or_formed = false;
        st->virt_entries = 0;
        st->virt_in_pos = false;
        st->virt_side = 0;
        st->virt_peak_mfe = 0.0;
    }

    if (s_id < 0) return sig;

    const int16_t mins = view.min_in_sess();
    if (mins < st->or_mins) {
        st->custom_or_h = std::max(st->custom_or_h, static_cast<double>(b0.h));
        st->custom_or_l = std::min(st->custom_or_l, static_cast<double>(b0.l));
    } else if (st->custom_or_h > -1e17 && st->custom_or_l < 1e17) {
        st->custom_or_formed = true;
    }

    bool or_ready = false;
    double or_h = 0.0, or_l = 0.0;
    if (st->or_mins == 15 || st->or_mins == 30 || st->or_mins == 45 || st->or_mins == 60) {
        or_ready = view.or_formed(st->or_mins);
        if (or_ready) {
            or_h = view.or_high(st->or_mins);
            or_l = view.or_low(st->or_mins);
        }
    } else {
        or_ready = st->custom_or_formed;
        or_h = st->custom_or_h;
        or_l = st->custom_or_l;
    }
    if (!or_ready) return sig;

    const double ir = inst_ratio(st->inst);
    const double ref_atr = 4.80 * ir;
    const double ratio_atr = (st->sess_atr_1h > 1e-6) ? (st->sess_atr_1h / ref_atr) : 1.0;
    const double raw_buf = (25.0 * ir) * std::pow(std::max(1e-6, ratio_atr), 1.25);
    const double buf = std::clamp(raw_buf, 8.0 * ir, 90.0 * ir);
    const double scale = price_scale(st->sess_open_px, st->inst);
    const double sl_pts = std::max(st->inst.tick_size * 4.0, st->base_sl_pts * ir * scale);

    const double buy_trig = or_h + buf;
    const double sell_trig = or_l - buf;

    // Advance virtual ORB state machine to track reentry_idx when min_reentry > 1
    if (st->min_reentry > 1) {
        const double bh = static_cast<double>(b0.h);
        const double bl = static_cast<double>(b0.l);
        const double bc = static_cast<double>(b0.c);
        if (st->virt_in_pos) {
            if ((st->virt_side == 1 && bl <= st->virt_sl_px) || (st->virt_side == -1 && bh >= st->virt_sl_px)) {
                st->virt_in_pos = false;
            } else {
                double fav = (st->virt_side == 1) ? (bh - st->virt_entry_px) : (st->virt_entry_px - bl);
                if (fav > st->virt_peak_mfe) st->virt_peak_mfe = fav;
                if (st->ratchet_trig_r > 0.0 && st->virt_peak_mfe >= st->ratchet_trig_r * sl_pts) {
                    double lock = std::max((st->ratchet_trig_r - st->ratchet_trail_r) * sl_pts,
                                           st->virt_peak_mfe - st->ratchet_trail_r * sl_pts);
                    double fl = (st->virt_side == 1) ? (st->virt_entry_px + lock) : (st->virt_entry_px - lock);
                    if (st->virt_side == 1) st->virt_sl_px = std::max(st->virt_sl_px, fl);
                    else st->virt_sl_px = std::min(st->virt_sl_px, fl);
                    if ((st->virt_side == 1 && bc <= st->virt_sl_px) || (st->virt_side == -1 && bc >= st->virt_sl_px)) {
                        st->virt_in_pos = false;
                    }
                }
            }
        }
        if (!st->virt_in_pos && st->virt_entries < st->min_reentry - 1) {
            if (bh >= buy_trig) {
                st->virt_entries++;
                st->virt_in_pos = true;
                st->virt_side = 1;
                st->virt_entry_px = buy_trig;
                st->virt_sl_px = buy_trig - sl_pts;
                st->virt_peak_mfe = 0.0;
            } else if (bl <= sell_trig) {
                st->virt_entries++;
                st->virt_in_pos = true;
                st->virt_side = -1;
                st->virt_entry_px = sell_trig;
                st->virt_sl_px = sell_trig + sl_pts;
                st->virt_peak_mfe = 0.0;
            }
            return sig;
        }
        if (st->virt_in_pos) return sig;
    }

    const int max_allowed = std::max(1, st->max_reentry - st->min_reentry + 1);
    if (view.in_position || view.sess_entries >= max_allowed) return sig;

    const double c_now = static_cast<double>(b0.c);
    if (st->dir_filter == 1) {
        sig.direction = 1;
        sig.trigger_px = buy_trig;
    } else if (st->dir_filter == -1) {
        sig.direction = -1;
        sig.trigger_px = sell_trig;
    } else {
        if (c_now >= buy_trig) {
            sig.direction = 1;
            sig.trigger_px = buy_trig;
        } else if (c_now <= sell_trig) {
            sig.direction = -1;
            sig.trigger_px = sell_trig;
        } else if ((buy_trig - c_now) <= (c_now - sell_trig)) {
            sig.direction = 1;
            sig.trigger_px = buy_trig;
        } else {
            sig.direction = -1;
            sig.trigger_px = sell_trig;
        }
    }

    sig.order_type = 1; // Resting Stop-Entry
    sig.sl_pts = sl_pts;
    sig.tp_pts = (st->rr_tp > 0.0) ? (st->rr_tp * sl_pts) : 0.0;
    sig.ratchet_trig_pts = (st->ratchet_trig_r > 0.0) ? (st->ratchet_trig_r * sl_pts) : 0.0;
    sig.ratchet_trail_pts = (st->ratchet_trail_r > 0.0) ? (st->ratchet_trail_r * sl_pts) : 0.0;
    sig.max_hold_bars = 180;
    sig.cooldown_bars = 1;
    return sig;
}

// ============================================================================
// FAMILY 3: Physics Batch 9 (phys_181 .. phys_200)
// ============================================================================
struct PhysB9State {
    int strat_id = 181;
    u11::InstrumentSpec inst = u11::get_instrument_spec("NQ");
    int or_mins = 30;
    double signal_thresh = 1.2;
    double target_r = 7.0;
    double be_trig_r = 1.20;
    double be_lock_r = 0.25;
    int max_entries = 2;

    size_t last_seen_bar = std::numeric_limits<size_t>::max();
    double kalman_x = 0.0;
    double kalman_p = 1.0;
    double hawkes_self = 0.0;
    double hawkes_cross = 0.0;
};

inline void init_b9_common(PhysB9State& st, int sid, const std::vector<double>& pv, const u11::InstrumentSpec* sp,
                           double def_th, double def_tgt, double def_bet, double def_bel) {
    st.strat_id = sid;
    st.inst = sp ? *sp : u11::get_instrument_spec("NQ");
    st.or_mins = std::max(15, static_cast<int>(std::round(p_or(pv, 0, 30.0))));
    st.signal_thresh = p_or(pv, 1, def_th);
    st.target_r = p_or(pv, 2, def_tgt);
    st.be_trig_r = p_or(pv, 3, def_bet);
    st.be_lock_r = p_or(pv, 4, def_bel);
    st.max_entries = std::max(1, static_cast<int>(std::round(p_or(pv, 5, 2.0))));
    st.last_seen_bar = std::numeric_limits<size_t>::max();
    st.kalman_x = 0.0;
    st.kalman_p = 1.0;
    st.hawkes_self = 0.0;
    st.hawkes_cross = 0.0;
}

inline void b9_step_bar(PhysB9State& st, const u11::CausalBarView& view, int lag, double& out_innov) {
    const auto& kb = view[lag];
    const auto& kb_prev = view[lag - 1];
    double cur_a = std::max(st.inst.tick_size, view.atr14(lag));
    if (st.kalman_x == 0.0) st.kalman_x = static_cast<double>(kb.c);

    st.kalman_p += 0.05;
    double K = st.kalman_p / (st.kalman_p + cur_a * cur_a * 0.25);
    out_innov = static_cast<double>(kb.c) - st.kalman_x;
    st.kalman_x += K * out_innov;
    st.kalman_p *= (1.0 - K);

    double es_a = std::max(1e-4, es_ref_atr14(view, st.inst, lag));
    const auto& es0 = es_ref_bar(view, st.inst, lag);
    const auto& es1 = es_ref_bar(view, st.inst, lag - 1);
    st.hawkes_self = st.hawkes_self * 0.85 + (std::abs(static_cast<double>(kb.c) - static_cast<double>(kb_prev.c)) > 1.2 * cur_a ? 1.0 : 0.0);
    st.hawkes_cross = st.hawkes_cross * 0.85 + (std::abs(static_cast<double>(es0.c) - static_cast<double>(es1.c)) > 1.2 * es_a ? 1.0 : 0.0);
}

inline u11::PluginSignal phys_b9_on_bar(const u11::CausalBarView& view, void* state) {
    auto* st = static_cast<PhysB9State*>(state);
    u11::PluginSignal sig{};

    const size_t cur_b = view.bar_index();
    double innovation = 0.0;
    if (st->last_seen_bar == std::numeric_limits<size_t>::max()) {
        size_t start_b = (cur_b > 120) ? (cur_b - 120) : 0;
        for (size_t k = start_b; k <= cur_b; ++k) {
            b9_step_bar(*st, view, -static_cast<int>(cur_b - k), innovation);
        }
    } else if (cur_b > st->last_seen_bar) {
        size_t start_b = std::max(st->last_seen_bar + 1, (cur_b > 240) ? (cur_b - 240) : 0);
        for (size_t k = start_b; k <= cur_b; ++k) {
            b9_step_bar(*st, view, -static_cast<int>(cur_b - k), innovation);
        }
    }
    st->last_seen_bar = cur_b;

    if (view.session_id() < 0 || !view.or_formed(st->or_mins)) return sig;
    if (view.in_position || view.sess_entries >= st->max_entries || cur_b < 15) return sig;

    const double or_h = view.or_high(st->or_mins);
    const double or_l = view.or_low(st->or_mins);
    const auto& b = view[0];
    const double bc = static_cast<double>(b.c);
    const double bo = static_cast<double>(b.o);
    const double bh = static_cast<double>(b.h);
    const double bl = static_cast<double>(b.l);
    const double bv = std::max(0.0, static_cast<double>(b.v));
    const double cur_a = std::max(st->inst.tick_size, view.atr14(0));
    const double scale = price_scale(bo, st->inst);

    const double es_change = static_cast<double>(es_ref_bar(view, st->inst, -1).c) - static_cast<double>(es_ref_bar(view, st->inst, -5).c);
    const double vol_ratio = static_cast<double>(view[-1].v) / (static_cast<double>(view[-10].v) + 1e-4);

    bool bull = false, bear = false;
    const int sid = st->strat_id;
    const double th = st->signal_thresh;

    if (sid == 181) {
        double dH = (static_cast<double>(view[-1].c) - static_cast<double>(view[-5].c)) * static_cast<double>(view[-1].v);
        double dS = std::log(std::max(0.0, static_cast<double>(view[-1].v)) / (std::max(0.0, static_cast<double>(view[-5].v)) + 1e-4) + 1.0);
        double dG = dH - cur_a * dS * 1000.0;
        if (bc > or_h && dG > 1000.0 * cur_a && es_change > 0.0 && vol_ratio > th) bull = true;
        if (bc < or_l && dG < -1000.0 * cur_a && es_change < 0.0 && vol_ratio > th) bear = true;
    } else if (sid == 182) {
        if (bc > or_h && innovation > th * cur_a && es_change > 0.0) bull = true;
        if (bc < or_l && innovation < -th * cur_a && es_change < 0.0) bear = true;
    } else if (sid == 183) {
        double hc = st->hawkes_self + st->hawkes_cross;
        if (bc > or_h && hc > th && es_change > 0.0) bull = true;
        if (bc < or_l && hc > th && es_change < 0.0) bear = true;
    } else if (sid == 184) {
        double kin = (bc - static_cast<double>(view[-3].c)) / cur_a;
        double E = 0.5 * kin * kin;
        if (bc > or_h && E > th && es_change > 0.0) bull = true;
        if (bc < or_l && E > th && es_change < 0.0) bear = true;
    } else if (sid == 185) {
        double p1 = std::abs(bc - static_cast<double>(view[-1].c)) / cur_a + 1e-4;
        double p2 = std::abs(static_cast<double>(view[-1].c) - static_cast<double>(view[-2].c)) / cur_a + 1e-4;
        double norm = p1 + p2;
        p1 /= norm; p2 /= norm;
        double tsallis = (1.0 - (std::pow(p1, 1.5) + std::pow(p2, 1.5))) / 0.5;
        if (bc > or_h && tsallis < th && es_change > 0.0) bull = true;
        if (bc < or_l && tsallis < th && es_change < 0.0) bear = true;
    } else if (sid == 186) {
        double conv = 0.0;
        for (int k = 1; k <= 8; ++k) conv += std::pow(static_cast<double>(k), -0.4) * (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
        if (bc > or_h && conv > th * cur_a && es_change > 0.0) bull = true;
        if (bc < or_l && conv < -th * cur_a && es_change < 0.0) bear = true;
    } else if (sid == 187) {
        double mean8 = 0.0;
        for (int k = 1; k <= 8; ++k) mean8 += static_cast<double>(view[-k].c);
        mean8 /= 8.0;
        double dev8 = 0.0;
        for (int k = 1; k <= 8; ++k) dev8 += std::abs(static_cast<double>(view[-k].c) - mean8);
        dev8 /= 8.0;
        double hurst = (dev8 > 1e-4) ? (std::log(std::max(1e-6, dev8 / cur_a)) / std::log(8.0) + 0.5) : 0.5;
        if (bc > or_h && hurst > th && es_change > 0.0) bull = true;
        if (bc < or_l && hurst > th && es_change < 0.0) bear = true;
    } else if (sid == 188) {
        double imf1 = bc - static_cast<double>(view[-2].c);
        double imf2 = static_cast<double>(view[-2].c) - static_cast<double>(view[-6].c);
        if (bc > or_h && imf1 > th * cur_a && imf2 > 0.6 * cur_a && es_change > 0.0) bull = true;
        if (bc < or_l && imf1 < -th * cur_a && imf2 < -0.6 * cur_a && es_change < 0.0) bear = true;
    } else if (sid == 189) {
        double d0 = std::abs(static_cast<double>(view[-4].c) - static_cast<double>(view[-8].c)) + 1e-4;
        double dt = std::abs(bc - static_cast<double>(view[-4].c)) + 1e-4;
        double ftle = 0.25 * std::log(dt / d0);
        if (bc > or_h && ftle > th && es_change > 0.0) bull = true;
        if (bc < or_l && ftle > th && es_change < 0.0) bear = true;
    } else if (sid == 190) {
        double vpin = std::abs(bc - bo) / (bh - bl + 1e-4);
        if (bc > or_h && vpin > th && vol_ratio > 1.25 && es_change > 0.0) bull = true;
        if (bc < or_l && vpin > th && vol_ratio > 1.25 && es_change < 0.0) bear = true;
    } else if (sid == 191) {
        double d1 = std::abs(bc - static_cast<double>(view[-2].c));
        double d2 = std::abs(static_cast<double>(view[-2].c) - static_cast<double>(view[-4].c));
        double betti_ratio = d1 / (d2 + 1e-4);
        if (bc > or_h && betti_ratio > th && es_change > 0.0) bull = true;
        if (bc < or_l && betti_ratio > th && es_change < 0.0) bear = true;
    } else if (sid == 192) {
        double es_a = std::max(1e-4, es_ref_atr14(view, st->inst, 0));
        double joint_drop = (bc - static_cast<double>(view[-3].c)) / cur_a +
                            (static_cast<double>(es_ref_bar(view, st->inst, 0).c) - static_cast<double>(es_ref_bar(view, st->inst, -3).c)) / es_a;
        if (bc > or_h && joint_drop > th && es_change > 0.0) bull = true;
        if (bc < or_l && joint_drop < -th && es_change < 0.0) bear = true;
    } else if (sid == 193) {
        bool state_was_chop = std::abs(static_cast<double>(view[-2].c) - static_cast<double>(view[-6].c)) < 1.0 * cur_a;
        bool state_now_trend = std::abs(bc - static_cast<double>(view[-2].c)) > th * cur_a;
        if (bc > or_h && state_was_chop && state_now_trend && es_change > 0.0) bull = true;
        if (bc < or_l && state_was_chop && state_now_trend && es_change < 0.0) bear = true;
    } else if (sid == 194) {
        double p_pulse = (bc - static_cast<double>(view[-2].c)) * bv / (cur_a * 1000.0);
        if (bc > or_h && p_pulse > th && vol_ratio > 1.15 && es_change > 0.0) bull = true;
        if (bc < or_l && p_pulse < -th && vol_ratio > 1.15 && es_change < 0.0) bear = true;
    } else if (sid == 195) {
        double vol_short = std::abs(bc - static_cast<double>(view[-2].c));
        double rough_ratio = vol_short / (cur_a + 1e-4);
        if (bc > or_h && rough_ratio > th && es_change > 0.0) bull = true;
        if (bc < or_l && rough_ratio > th && es_change < 0.0) bear = true;
    } else if (sid == 196) {
        double es_a = std::max(1e-4, es_ref_atr14(view, st->inst, 0));
        double norm_nq = (bc - static_cast<double>(view[-4].c)) / cur_a;
        double norm_es = (static_cast<double>(es_ref_bar(view, st->inst, 0).c) - static_cast<double>(es_ref_bar(view, st->inst, -4).c)) / es_a;
        if (bc > or_h && norm_nq > th && norm_es > 0.8 && es_change > 0.0) bull = true;
        if (bc < or_l && norm_nq < -th && norm_es < -0.8 && es_change < 0.0) bear = true;
    } else if (sid == 197) {
        double es_a = std::max(1e-4, es_ref_atr14(view, st->inst, 0));
        double te = (static_cast<double>(es_ref_bar(view, st->inst, -1).c) - static_cast<double>(es_ref_bar(view, st->inst, -3).c)) *
                    (bc - static_cast<double>(view[-1].c));
        if (bc > or_h && te > th * cur_a * es_a && es_change > 0.0) bull = true;
        if (bc < or_l && te > th * cur_a * es_a && es_change < 0.0) bear = true;
    } else if (sid == 198) {
        double d_a = bc - static_cast<double>(view[-2].c);
        double d_b = static_cast<double>(view[-1].c) - static_cast<double>(view[-3].c);
        double wv = d_a * d_a + d_b * d_b;
        if (bc > or_h && wv > th * cur_a * cur_a && es_change > 0.0) bull = true;
        if (bc < or_l && wv > th * cur_a * cur_a && es_change < 0.0) bear = true;
    } else if (sid == 199) {
        int s1 = (bc > static_cast<double>(view[-1].c)) ? 1 : 0;
        int s2 = (view[-1].c > view[-2].c) ? 1 : 0;
        int s3 = (view[-2].c > view[-3].c) ? 1 : 0;
        bool aligned = (s1 == s2 && s2 == s3);
        if (bc > or_h && aligned && s1 == 1 && es_change > th) bull = true;
        if (bc < or_l && aligned && s1 == 0 && es_change < -th) bear = true;
    } else if (sid == 200) {
        double r_bif = (bh - bl) / (cur_a + 1e-4);
        if (bc > or_h && r_bif > th && es_change > 0.0) bull = true;
        if (bc < or_l && r_bif > th && es_change < 0.0) bear = true;
    }

    if (bull || bear) {
        const double or_mid = 0.5 * (or_h + or_l);
        const double raw_r = bull ? (bc - or_mid) : (or_mid - bc);
        const double min_r = 0.0025 * st->inst.baseline_index * scale;
        const double max_r = 0.0060 * st->inst.baseline_index * scale;
        const double r_pts = std::max(st->inst.tick_size * 4.0, std::clamp(raw_r, min_r, max_r));

        sig.direction = bull ? 1 : -1;
        sig.order_type = 0;
        sig.sl_pts = r_pts;
        sig.tp_pts = st->target_r * r_pts;
        sig.be_trig_pts = (st->be_trig_r > 0.0) ? (st->be_trig_r * r_pts) : 0.0;
        sig.be_offset_pts = st->be_lock_r * r_pts;
        sig.max_hold_bars = 240;
        sig.cooldown_bars = 1;
    }
    return sig;
}

// ============================================================================
// FAMILY 4: Physics Batch 10 & 11 (phys_201 .. phys_240)
// ============================================================================
struct PhysB1011State {
    int strat_id = 201;
    u11::InstrumentSpec inst = u11::get_instrument_spec("NQ");
    double signal_thresh = 1.4;
    double vol_mult = 1.1;
    double min_r_pts = 4.0;
    double max_r_pts = 8.0;
    double target_r = 7.5;
    int max_entries = 2;
};

inline void init_b1011_common(PhysB1011State& st, int sid, const std::vector<double>& pv, const u11::InstrumentSpec* sp,
                              double def_th, double def_vm, double def_minr, double def_maxr, double def_tgt) {
    st.strat_id = sid;
    st.inst = sp ? *sp : u11::get_instrument_spec("NQ");
    st.signal_thresh = p_or(pv, 0, def_th);
    st.vol_mult = p_or(pv, 1, def_vm);
    st.min_r_pts = p_or(pv, 2, def_minr);
    st.max_r_pts = std::max(st.min_r_pts, p_or(pv, 3, def_maxr));
    st.target_r = p_or(pv, 4, def_tgt);
    st.max_entries = std::max(1, static_cast<int>(std::round(p_or(pv, 5, 2.0))));
}

inline u11::PluginSignal phys_b1011_on_bar(const u11::CausalBarView& view, void* state) {
    const auto* st = static_cast<const PhysB1011State*>(state);
    u11::PluginSignal sig{};
    if (view.session_id() < 0 || view.min_in_sess() <= 5) return sig;
    if (view.in_position || view.sess_entries >= st->max_entries || view.bar_index() < 35) return sig;

    const auto& b = view[0];
    const double bc = static_cast<double>(b.c);
    const double bo = static_cast<double>(b.o);
    const double bh = static_cast<double>(b.h);
    const double bl = static_cast<double>(b.l);
    const double bv = std::max(0.0, static_cast<double>(b.v));
    const double vwap = (view.session_vwap(0) > 0.0) ? view.session_vwap(0) : bc;
    const double cur_a = std::max(st->inst.tick_size, view.atr14(0));
    const double ir = inst_ratio(st->inst);
    const double scale = price_scale(bo, st->inst);

    const auto& r1_0 = ref1_bar(view, st->inst, 0);
    const auto& r1_3 = ref1_bar(view, st->inst, -3);
    const auto& r1_4 = ref1_bar(view, st->inst, -4);
    const auto& r1_5 = ref1_bar(view, st->inst, -5);
    const auto& r1_6 = ref1_bar(view, st->inst, -6);
    const double r1_atr = std::max(1e-4, ref1_atr14(view, st->inst, 0));

    bool bull = false, bear = false;
    double proposed_r = 5.5 * ir * scale;
    const int sid = st->strat_id;
    const double th = st->signal_thresh;
    const double vm = st->vol_mult;

    if (sid == 201) {
        double d5 = bc - static_cast<double>(view[-5].c);
        double ref_d5 = static_cast<double>(r1_0.c) - static_cast<double>(r1_5.c);
        if (d5 > th * cur_a && ref_d5 > 0.3 * r1_atr && bc > vwap && bv > vm * static_cast<double>(view[-5].v)) {
            bull = true; proposed_r = bc - static_cast<double>(view[-2].l);
        } else if (d5 < -th * cur_a && ref_d5 < -0.3 * r1_atr && bc < vwap && bv > vm * static_cast<double>(view[-5].v)) {
            bear = true; proposed_r = static_cast<double>(view[-2].h) - bc;
        }
    } else if (sid == 202) {
        double sm = 0.0;
        for (int k = 1; k <= 30; ++k) sm += static_cast<double>(view[-k].c);
        sm /= 30.0;
        double num = 0.0, den = 0.0;
        for (int k = 1; k <= 29; ++k) {
            double xc = static_cast<double>(view[-k].c) - sm;
            double xp = static_cast<double>(view[-k - 1].c) - sm;
            num += xc * xp;
            den += xp * xp;
        }
        double beta = (den > 1e-4) ? (num / den) : 0.0;
        double dev = bc - sm;
        if (beta > th && std::abs(dev) > 1.8 * cur_a && bv > vm * static_cast<double>(view[-5].v)) {
            if (dev > 0.0 && bc > bo) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (dev < 0.0 && bc < bo) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 203) {
        double d_sym = bc - static_cast<double>(view[-6].c);
        double d_ref = static_cast<double>(r1_0.c) - static_cast<double>(r1_6.c);
        if (d_sym > th * cur_a && d_ref > 0.4 * r1_atr && bc > vwap && bv > vm * static_cast<double>(view[-4].v)) {
            bull = true; proposed_r = bc - static_cast<double>(view[-2].l);
        } else if (d_sym < -th * cur_a && d_ref < -0.4 * r1_atr && bc < vwap && bv > vm * static_cast<double>(view[-4].v)) {
            bear = true; proposed_r = static_cast<double>(view[-2].h) - bc;
        }
    } else if (sid == 204) {
        double x_est = static_cast<double>(view[-15].c);
        double p_cov = 10.0, q_proc = 0.5, r_meas = 2.0 * ir * ir;
        double innov = 0.0, s_var = 1.0;
        for (int k = 14; k >= 0; --k) {
            p_cov += q_proc;
            innov = static_cast<double>(view[-k].c) - x_est;
            s_var = p_cov + r_meas;
            double k_gain = p_cov / s_var;
            x_est += k_gain * innov;
            p_cov *= (1.0 - k_gain);
        }
        double norm_innov = innov / std::sqrt(std::max(1e-6, s_var));
        if (std::abs(norm_innov) > th && bv > vm * static_cast<double>(view[-4].v)) {
            if (norm_innov > 0.0 && bc > bo) { bull = true; proposed_r = bc - bl; }
            else if (norm_innov < 0.0 && bc < bo) { bear = true; proposed_r = bh - bc; }
        }
    } else if (sid == 205) {
        int bull_run = 0, bear_run = 0;
        for (int k = 0; k < 4; ++k) {
            if (view[-k].c > view[-k].o) bull_run++;
            if (view[-k].c < view[-k].o) bear_run++;
        }
        if (bull_run >= 3 && bc > static_cast<double>(view[-3].h) && (bc - bo) > th * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            bull = true; proposed_r = bc - static_cast<double>(view[-2].l);
        } else if (bear_run >= 3 && bc < static_cast<double>(view[-3].l) && (bo - bc) > th * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            bear = true; proposed_r = static_cast<double>(view[-2].h) - bc;
        }
    } else if (sid == 206) {
        double d1[4];
        for (int k = 0; k < 4; ++k) {
            d1[k] = (static_cast<double>(view[-2 * k].c) - static_cast<double>(view[-2 * k - 1].c)) * 0.70710678;
        }
        double e_d1 = 0.0;
        for (int k = 0; k < 4; ++k) e_d1 += d1[k] * d1[k];
        double e_tot = 0.0;
        for (int k = 0; k < 8; ++k) {
            double diff = static_cast<double>(view[-k].c) - static_cast<double>(view[-7].c);
            e_tot += diff * diff;
        }
        double lf_ratio = (e_tot > 1e-4) ? (1.0 - (e_d1 / e_tot)) : 0.0;
        if (lf_ratio > th && std::abs(bc - static_cast<double>(view[-7].c)) > vm * cur_a) {
            if (bc > static_cast<double>(view[-7].c)) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 207) {
        double p0 = static_cast<double>(view[-6].c);
        double travel = std::abs(bc - p0);
        if (travel > th * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > p0 && bc > bo) { bull = true; proposed_r = bc - bl; }
            else if (bc < p0 && bc < bo) { bear = true; proposed_r = bh - bc; }
        }
    } else if (sid == 208) {
        double buy_v = 0.0, sell_v = 0.0;
        for (int k = 0; k < 12; ++k) {
            const auto& bk = view[-k];
            double rng = static_cast<double>(bk.h) - static_cast<double>(bk.l) + 1e-4;
            buy_v += static_cast<double>(bk.v) * ((static_cast<double>(bk.c) - static_cast<double>(bk.l)) / rng);
            sell_v += static_cast<double>(bk.v) * ((static_cast<double>(bk.h) - static_cast<double>(bk.c)) / rng);
        }
        double vpin = std::abs(buy_v - sell_v) / (buy_v + sell_v + 1e-4);
        double r_body = static_cast<double>(r1_0.c) - static_cast<double>(r1_0.o);
        if (vpin > th && std::abs(bc - static_cast<double>(view[-6].c)) > vm * cur_a && (bc - bo) * r_body > 0.0) {
            if (buy_v > sell_v && bc > bo) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (sell_v > buy_v && bc < bo) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 209) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (int k = 0; k < 16; ++k) {
            double yc = static_cast<double>(view[-15 + k].c);
            sx += k; sy += yc; sxx += k * k; sxy += k * yc;
        }
        double slope = (16.0 * sxy - sx * sy) / (16.0 * sxx - sx * sx);
        double fit_err = 0.0;
        for (int k = 0; k < 16; ++k) {
            double fitted = (sy / 16.0) + slope * (k - 7.5);
            double err = static_cast<double>(view[-15 + k].c) - fitted;
            fit_err += err * err;
        }
        fit_err = std::sqrt(fit_err / 16.0);
        double pers_ratio = std::abs(slope * 16.0) / (fit_err + 1e-4);
        if (pers_ratio > th && std::abs(bc - static_cast<double>(view[-15].c)) > vm * cur_a) {
            if (slope > 0.0 && bc > vwap && bc > bo) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (slope < 0.0 && bc < vwap && bc < bo) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 210) {
        double d0 = std::hypot(static_cast<double>(view[-2].c) - static_cast<double>(view[-6].c),
                               static_cast<double>(view[-3].c) - static_cast<double>(view[-7].c));
        double d1 = std::hypot(bc - static_cast<double>(view[-4].c),
                               static_cast<double>(view[-1].c) - static_cast<double>(view[-5].c));
        double lyap = (d0 > 1e-3 && d1 > 1e-6) ? std::log(d1 / d0) : 0.0;
        double r_body = static_cast<double>(r1_0.c) - static_cast<double>(r1_0.o);
        if (lyap > th && std::abs(bc - static_cast<double>(view[-4].c)) > vm * cur_a && (bc - bo) * r_body > 0.0) {
            if (bc > bo && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < bo && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 211) {
        double v_arr[10];
        double sum_v = 0.0;
        for (int k = 0; k < 10; ++k) { v_arr[k] = std::max(0.0, static_cast<double>(view[-k].v)); sum_v += v_arr[k]; }
        double diff_sum = 0.0;
        for (int k1 = 0; k1 < 10; ++k1)
            for (int k2 = 0; k2 < 10; ++k2)
                diff_sum += std::abs(v_arr[k1] - v_arr[k2]);
        double gini = diff_sum / (2.0 * 10.0 * sum_v + 1e-4);
        double d5 = bc - static_cast<double>(view[-5].c);
        double r_body = static_cast<double>(r1_0.c) - static_cast<double>(r1_0.o);
        if (gini > th && std::abs(d5) > vm * cur_a && (bc - bo) * r_body > 0.0) {
            if (d5 > 0.0 && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (d5 < 0.0 && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 212) {
        double v_sym = cur_a / std::max(1.0, bc);
        double v_ref = r1_atr / std::max(1.0, static_cast<double>(r1_0.c));
        double ratio = v_sym / (v_ref + 1e-6);
        if (ratio > th && std::abs(bc - static_cast<double>(view[-5].c)) > vm * cur_a) {
            if (bc > static_cast<double>(view[-5].c) && r1_0.c > r1_5.c) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-5].c) && r1_0.c < r1_5.c) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 213) {
        double h15 = static_cast<double>(view[-1].h), l15 = static_cast<double>(view[-1].l);
        for (int k = 2; k <= 15; ++k) {
            h15 = std::max(h15, static_cast<double>(view[-k].h));
            l15 = std::min(l15, static_cast<double>(view[-k].l));
        }
        if (bc > h15 && static_cast<double>(view[-1].h) <= h15 && (bc - bo) > th * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            bull = true; proposed_r = bc - bl;
        } else if (bc < l15 && static_cast<double>(view[-1].l) >= l15 && (bo - bc) > th * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            bear = true; proposed_r = bh - bc;
        }
    } else if (sid == 214) {
        double h3 = std::max({static_cast<double>(view[-1].h), static_cast<double>(view[-2].h), static_cast<double>(view[-3].h)});
        double l3 = std::min({static_cast<double>(view[-1].l), static_cast<double>(view[-2].l), static_cast<double>(view[-3].l)});
        if (bc > vwap && bc > h3 && (bc - bo) > th * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            bull = true; proposed_r = bc - l3;
        } else if (bc < vwap && bc < l3 && (bo - bc) > th * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            bear = true; proposed_r = h3 - bc;
        }
    } else if (sid == 215) {
        const auto& r2_0 = ref2_bar(view, st->inst, 0);
        const auto& r2_4 = ref2_bar(view, st->inst, -4);
        const double r2_atr = std::max(1e-4, ref2_atr14(view, st->inst, 0));
        double d_sym = bc - static_cast<double>(view[-4].c);
        double d_r1 = static_cast<double>(r1_0.c) - static_cast<double>(r1_4.c);
        double d_r2 = static_cast<double>(r2_0.c) - static_cast<double>(r2_4.c);
        bool sb = (d_sym > th * cur_a) && (d_r1 > 0.35 * r1_atr) && (d_r2 > 0.35 * r2_atr);
        bool sr = (d_sym < -th * cur_a) && (d_r1 < -0.35 * r1_atr) && (d_r2 < -0.35 * r2_atr);
        if (sb && bc > vwap && bv > vm * static_cast<double>(view[-3].v)) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
        else if (sr && bc < vwap && bv > vm * static_cast<double>(view[-3].v)) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
    } else if (sid == 216) {
        double temp = 0.0;
        for (int k = 1; k <= 20; ++k) {
            double v = std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
            temp += v * v;
        }
        temp = std::sqrt(temp / 20.0);
        double curr_v = std::abs(bc - bo);
        double r_body = static_cast<double>(r1_0.c) - static_cast<double>(r1_0.o);
        if (curr_v > th * temp && bv > vm * static_cast<double>(view[-3].v) && (bc - bo) * r_body > 0.0) {
            if (bc > bo && bc > vwap) { bull = true; proposed_r = bc - bl; }
            else if (bc < bo && bc < vwap) { bear = true; proposed_r = bh - bc; }
        }
    } else if (sid == 217) {
        double path_len = 0.0;
        for (int k = 0; k < 6; ++k) path_len += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
        double direct_disp = std::abs(bc - static_cast<double>(view[-6].c));
        double efficiency = direct_disp / (path_len + 1e-4);
        if (efficiency > th && direct_disp > 2.0 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-6].c)) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 218) {
        double r1_val = 0.0, r2_val = 0.0;
        for (int k = 1; k <= 20; ++k) {
            double d0 = static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c);
            double d1 = static_cast<double>(view[-k - 1].c) - static_cast<double>(view[-k - 2].c);
            double d2 = static_cast<double>(view[-k - 2].c) - static_cast<double>(view[-k - 3].c);
            r1_val += d0 * d1; r2_val += d0 * d2;
        }
        if (r1_val > 0.0 && r2_val > 0.0 && std::abs(bc - static_cast<double>(view[-5].c)) > th * cur_a) {
            if (bc > static_cast<double>(view[-5].c) && bc > bo) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-5].c) && bc < bo) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 219) {
        double d4 = bc - static_cast<double>(view[-4].c);
        double prev_body = std::abs(static_cast<double>(view[-1].c) - static_cast<double>(view[-1].o));
        double curr_body = std::abs(bc - bo);
        if (prev_body < 0.50 * cur_a && curr_body > th * cur_a && std::abs(d4) > 1.4 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > bo && r1_0.c > r1_0.o && bc > vwap) { bull = true; proposed_r = bc - bl; }
            else if (bc < bo && r1_0.c < r1_0.o && bc < vwap) { bear = true; proposed_r = bh - bc; }
        }
    } else if (sid == 220) {
        double m1 = 0.0, v1 = 0.0, m2 = 0.0, v2 = 0.0;
        for (int k = 0; k < 6; ++k) m1 += (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
        m1 /= 6.0;
        for (int k = 0; k < 6; ++k) { double d = (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c)) - m1; v1 += d * d; }
        v1 = std::sqrt(v1 / 6.0 + 1e-4);
        for (int k = 6; k < 12; ++k) m2 += (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
        m2 /= 6.0;
        for (int k = 6; k < 12; ++k) { double d = (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c)) - m2; v2 += d * d; }
        v2 = std::sqrt(v2 / 6.0 + 1e-4);
        double ds = std::sqrt((m1 - m2) * (m1 - m2) / (v2 * v2) + 2.0 * (v1 - v2) * (v1 - v2) / (v2 * v2));
        if (ds > th && std::abs(bc - static_cast<double>(view[-6].c)) > 1.1 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-6].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-6].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 221) {
        double v_speed = std::abs(bc - static_cast<double>(view[-3].c));
        double friction = 0.0;
        for (int k = 0; k < 4; ++k) {
            friction += (static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l)) -
                        std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k].o));
        }
        double reynolds = v_speed / (friction + 1e-4);
        if (reynolds > th && std::abs(bc - static_cast<double>(view[-4].c)) > 1.5 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-4].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-4].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 222) {
        double dp = std::abs(bc - static_cast<double>(view[-5].c));
        double c_speed = th * cur_a;
        double s2 = dp * dp - c_speed * c_speed * 25.0;
        if (s2 > 0.0 && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-5].c) && r1_0.c > r1_5.c && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-5].c) && r1_0.c < r1_5.c && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 223) {
        double var1 = std::abs(bc - static_cast<double>(view[-1].c));
        double var16 = std::abs(bc - static_cast<double>(view[-16].c)) / 4.0;
        double rg_ratio = var16 / (var1 + 1e-4);
        if (rg_ratio > th && std::abs(bc - static_cast<double>(view[-6].c)) > vm * cur_a) {
            if (bc > static_cast<double>(view[-6].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-6].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 224) {
        double time_avg = 0.0;
        for (int k = 0; k < 20; ++k) time_avg += (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
        time_avg /= 20.0;
        double ens_var = 0.0;
        for (int k = 0; k < 20; ++k) {
            double d = (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c)) - time_avg;
            ens_var += d * d;
        }
        ens_var = std::sqrt(ens_var / 20.0 + 1e-4);
        double curr_flow = bc - static_cast<double>(view[-3].c);
        if (std::abs(curr_flow) > th * ens_var && bv > vm * static_cast<double>(view[-4].v)) {
            if (curr_flow > 0.0 && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (curr_flow < 0.0 && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 225) {
        bool j1 = std::abs(bc - static_cast<double>(view[-3].c)) > th * cur_a;
        bool j2 = std::abs(static_cast<double>(r1_0.c) - static_cast<double>(r1_3.c)) > 0.35 * r1_atr;
        if (j1 && j2 && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-3].c) && r1_0.c > r1_3.c && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-3].c) && r1_0.c < r1_3.c && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 226) {
        double v_vol = 0.0;
        for (int k = 0; k < 10; ++k) {
            double rng = static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l);
            double rng_prev = static_cast<double>(view[-k - 1].h) - static_cast<double>(view[-k - 1].l);
            v_vol += std::abs(rng - rng_prev);
        }
        v_vol /= 10.0;
        if (v_vol < th * cur_a && std::abs(bc - static_cast<double>(view[-4].c)) > 1.6 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > static_cast<double>(view[-4].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-4].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 227) {
        int degree = 0;
        for (int k = 1; k <= 7; ++k) {
            if (bh > static_cast<double>(view[-k].h) && bl < static_cast<double>(view[-k].l)) degree += 2;
            else if (bh > static_cast<double>(view[-k].h) || bl < static_cast<double>(view[-k].l)) degree++;
        }
        if (static_cast<double>(degree) >= th && std::abs(bc - bo) > 1.2 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > bo && bc > vwap) { bull = true; proposed_r = bc - bl; }
            else if (bc < bo && bc < vwap) { bear = true; proposed_r = bh - bc; }
        }
    } else if (sid == 228) {
        double c1 = std::abs(bc - static_cast<double>(view[-1].c));
        double c2 = std::abs(static_cast<double>(view[-1].c) - static_cast<double>(view[-2].c));
        double c3 = std::abs(static_cast<double>(view[-2].c) - static_cast<double>(view[-3].c));
        bool mono_up = (bc > static_cast<double>(view[-1].c) && view[-1].c > view[-2].c && view[-2].c > view[-3].c);
        bool mono_dn = (bc < static_cast<double>(view[-1].c) && view[-1].c < view[-2].c && view[-2].c < view[-3].c);
        if ((mono_up || mono_dn) && (c1 + c2 + c3) > th * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (mono_up && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (mono_dn && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 229) {
        double market_rate = bv / (bh - bl + 1e-4);
        double avg_mr = 0.0;
        for (int k = 1; k <= 10; ++k) avg_mr += static_cast<double>(view[-k].v) / (static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l) + 1e-4);
        avg_mr /= 10.0;
        double ti = market_rate / (avg_mr + 1e-4);
        if (ti > th && std::abs(bc - static_cast<double>(view[-3].c)) > vm * cur_a) {
            if (bc > static_cast<double>(view[-3].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-3].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 230) {
        int hvb = 0;
        for (int k = 0; k < 6; ++k) if (view[-k].v > view[-k - 1].v) hvb++;
        double disp = std::abs(bc - static_cast<double>(view[-6].c));
        if (static_cast<double>(hvb) >= th && disp > vm * cur_a) {
            if (bc > static_cast<double>(view[-6].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-6].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 231) {
        double d_fast = std::abs(bc - static_cast<double>(view[-3].c));
        double d_slow = std::abs(static_cast<double>(view[-3].c) - static_cast<double>(view[-9].c)) / 2.0;
        double kl_accel = d_fast / (d_slow + 1e-4);
        if (kl_accel > th && std::abs(bc - static_cast<double>(view[-4].c)) > 1.5 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > static_cast<double>(view[-4].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-4].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 232) {
        int repeats = 0;
        for (int k = 0; k < 5; ++k) {
            if ((view[-k].c > view[-k].o) == (view[-k - 1].c > view[-k - 1].o)) repeats++;
        }
        if (static_cast<double>(repeats) >= th && std::abs(bc - static_cast<double>(view[-5].c)) > 1.8 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-5].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-5].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 233) {
        double range_inner = 0.0;
        for (int k = 2; k <= 8; ++k) range_inner += (static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l));
        range_inner /= 7.0;
        double rupture = (bh - bl) / (range_inner + 1e-4);
        if (rupture > th && std::abs(bc - bo) > 1.4 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > bo && bc > vwap) { bull = true; proposed_r = bc - bl; }
            else if (bc < bo && bc < vwap) { bear = true; proposed_r = bh - bc; }
        }
    } else if (sid == 234) {
        double lf_power = std::abs(bc - static_cast<double>(view[-8].c));
        double hf_noise = 0.0;
        for (int k = 0; k < 8; ++k) hf_noise += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
        double power_ratio = lf_power / (hf_noise + 1e-4);
        if (power_ratio > th && lf_power > 2.0 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-8].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-8].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 235) {
        double flight = std::abs(bc - static_cast<double>(view[-2].c));
        double med_flight = 0.0;
        for (int k = 2; k <= 14; ++k) med_flight += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 2].c));
        med_flight /= 13.0;
        if (flight > th * med_flight && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > static_cast<double>(view[-2].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-2].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 236) {
        int spins = 0;
        for (int k = 0; k < 6; ++k) spins += (view[-k].c > view[-k].o ? 1 : -1);
        if (std::abs(spins) >= static_cast<int>(std::round(th)) && std::abs(bc - static_cast<double>(view[-5].c)) > 1.6 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (spins > 0 && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (spins < 0 && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 237) {
        double curr_j = (bc - static_cast<double>(view[-4].c)) * bv;
        double past_j = 0.0;
        for (int k = 4; k < 16; ++k) past_j += std::abs((static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 4].c)) * static_cast<double>(view[-k].v));
        past_j /= 12.0;
        if (std::abs(curr_j) > th * past_j && std::abs(bc - static_cast<double>(view[-4].c)) > vm * cur_a) {
            if (curr_j > 0.0 && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (curr_j < 0.0 && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 238) {
        double m4 = 0.0, m2 = 0.0;
        for (int k = 1; k <= 16; ++k) {
            double diff = static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c);
            m2 += diff * diff;
            m4 += diff * diff * diff * diff;
        }
        m2 /= 16.0; m4 /= 16.0;
        double kurt = m4 / (m2 * m2 + 1e-4);
        if (kurt > th && std::abs(bc - static_cast<double>(view[-3].c)) > 1.6 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > static_cast<double>(view[-3].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-3].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 239) {
        double speed = std::abs(bc - static_cast<double>(view[-3].c)) / 3.0;
        double prev_speed = std::abs(static_cast<double>(view[-3].c) - static_cast<double>(view[-9].c)) / 6.0;
        if (speed > th * prev_speed && speed > 0.5 * cur_a && bv > vm * static_cast<double>(view[-3].v)) {
            if (bc > static_cast<double>(view[-3].c) && bc > vwap) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-3].c) && bc < vwap) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    } else if (sid == 240) {
        double p1_val = std::abs(bc - static_cast<double>(view[-1].c));
        double p2_val = std::abs(static_cast<double>(view[-1].c) - static_cast<double>(view[-2].c));
        double p4_val = std::abs(bc - static_cast<double>(view[-4].c));
        if (p4_val > th * (p1_val + p2_val) && p4_val > 1.1 * cur_a && bv > vm * static_cast<double>(view[-4].v)) {
            if (bc > static_cast<double>(view[-4].c) && bc > vwap && r1_0.c > r1_4.c) { bull = true; proposed_r = bc - static_cast<double>(view[-2].l); }
            else if (bc < static_cast<double>(view[-4].c) && bc < vwap && r1_0.c < r1_4.c) { bear = true; proposed_r = static_cast<double>(view[-2].h) - bc; }
        }
    }

    if (bull || bear) {
        const double min_r = st->min_r_pts * ir * scale;
        const double max_r = st->max_r_pts * ir * scale;
        const double r_pts = std::max(st->inst.tick_size * 4.0, std::clamp(proposed_r, min_r, max_r));
        sig.direction = bull ? 1 : -1;
        sig.order_type = 0;
        sig.sl_pts = r_pts;
        sig.tp_pts = st->target_r * r_pts;
        sig.max_hold_bars = 360;
        sig.cooldown_bars = 1;
    }
    return sig;
}

// ============================================================================
// FAMILY 5: Instagram Reels Matrix (kernel_reel_1/2/6 + reel46_01..46)
// ============================================================================
struct ReelsState {
    int reel_id = 1; // -1=kernel_reel_1, -2=kernel_reel_2, -6=kernel_reel_6, 1..46=reel46_XX
    u11::InstrumentSpec inst = u11::get_instrument_spec("NQ");
    double sig_param = 1.0;
    double sl_pts = 20.0;
    double rr_tp = 2.0;
    double be_trig_r = 0.0;
    double ratchet_trig_r = 0.0;
    double ratchet_trail_r = 0.0;
    int max_trades = 2;

    // Causal backfill & daily structural accumulators (strictly lag <= 0)
    size_t last_seen_bar = std::numeric_limits<size_t>::max();
    int32_t cur_cme_dt = -1;
    int16_t prev_et_min = -1;
    double cur_day_h = -1e18, cur_day_l = 1e18, cur_day_c = 0.0;
    double pdh = 0.0, pdl = 0.0, pdc = 0.0;
    double prev_pdh = 0.0, prev_pdl = 0.0;
    bool has_pd = false, has_prev_pd = false;

    double pm_h = -1e18, pm_l = 1e18; bool has_pm = false;
    double asia_h = -1e18, asia_l = 1e18; bool has_asia = false;
    double on_h = -1e18, on_l = 1e18; bool has_on = false;
    double init_h = -1e18, init_l = 1e18; bool has_init = false;
    double c5_h = -1e18, c5_l = 1e18; bool has_c5 = false;
    double h1_h = -1e18, h1_l = 1e18; bool has_h1 = false;
    double r30_h = -1e18, r30_l = 1e18; bool has_r30 = false;
    double c4_h = -1e18, c4_l = 1e18; bool has_c4 = false;
    double m10_h = -1e18, m10_l = 1e18; bool has_m10 = false;
    double pre_h = -1e18, pre_l = 1e18; bool has_pre = false;
    double op15_h = -1e18, op15_l = 1e18; bool has_op15 = false;
    double fomc_h = -1e18, fomc_l = 1e18; bool has_fomc = false;
    bool fomc_swept_h = false, fomc_swept_l = false;

    double mid_open = 0.0, ny_open = 0.0;
    double on_sum = 0.0; int on_cnt = 0;
    bool found_fvg_today = false;
    int day_trades_issued = 0;

    s546::RollingWindow vol_win{20};
};

inline void init_reels_common(ReelsState& st, int rid, const std::vector<double>& pv, const u11::InstrumentSpec* sp,
                              double def_sig, double def_sl, double def_rr, double def_be, double def_rt, double def_rtr, int def_mt) {
    st.reel_id = rid;
    st.inst = sp ? *sp : u11::get_instrument_spec("NQ");
    st.sig_param = p_or(pv, 0, def_sig);
    st.sl_pts = p_or(pv, 1, def_sl);
    st.rr_tp = p_or(pv, 2, def_rr);
    st.be_trig_r = p_or(pv, 3, def_be);
    st.ratchet_trig_r = p_or(pv, 4, def_rt);
    st.ratchet_trail_r = p_or(pv, 5, def_rtr);
    st.max_trades = std::max(1, static_cast<int>(std::round(p_or(pv, 6, static_cast<double>(def_mt)))));
    st.last_seen_bar = std::numeric_limits<size_t>::max();
    st.vol_win.reset(20);
}

inline void reels_step_bar(ReelsState& st, const u11::CausalBarView& view, int lag, int16_t& out_et) {
    const auto& kb = view[lag];
    int16_t et = 0;
    int32_t cme_dt = 0;
    u11::decode_et_and_cme_date(kb.ts_sec, et, cme_dt);
    out_et = et;

    if (cme_dt != st.cur_cme_dt) {
        if (st.cur_cme_dt >= 0) {
            if (st.has_pd) {
                st.prev_pdh = st.pdh;
                st.prev_pdl = st.pdl;
                st.has_prev_pd = true;
            }
            st.pdh = st.cur_day_h;
            st.pdl = st.cur_day_l;
            st.pdc = st.cur_day_c;
            st.has_pd = true;
        }
        st.cur_cme_dt = cme_dt;
        st.cur_day_h = static_cast<double>(kb.h);
        st.cur_day_l = static_cast<double>(kb.l);
        st.cur_day_c = static_cast<double>(kb.c);

        st.pm_h = -1e18; st.pm_l = 1e18; st.has_pm = false;
        st.asia_h = -1e18; st.asia_l = 1e18; st.has_asia = false;
        st.on_h = -1e18; st.on_l = 1e18; st.has_on = false;
        st.init_h = -1e18; st.init_l = 1e18; st.has_init = false;
        st.c5_h = -1e18; st.c5_l = 1e18; st.has_c5 = false;
        st.h1_h = -1e18; st.h1_l = 1e18; st.has_h1 = false;
        st.r30_h = -1e18; st.r30_l = 1e18; st.has_r30 = false;
        st.c4_h = -1e18; st.c4_l = 1e18; st.has_c4 = false;
        st.m10_h = -1e18; st.m10_l = 1e18; st.has_m10 = false;
        st.pre_h = -1e18; st.pre_l = 1e18; st.has_pre = false;
        st.op15_h = -1e18; st.op15_l = 1e18; st.has_op15 = false;
        st.fomc_h = -1e18; st.fomc_l = 1e18; st.has_fomc = false;
        st.fomc_swept_h = false; st.fomc_swept_l = false;
        st.mid_open = 0.0; st.ny_open = 0.0;
        st.on_sum = 0.0; st.on_cnt = 0;
        st.found_fvg_today = false;
        st.day_trades_issued = 0;
    } else {
        st.cur_day_h = std::max(st.cur_day_h, static_cast<double>(kb.h));
        st.cur_day_l = std::min(st.cur_day_l, static_cast<double>(kb.l));
        st.cur_day_c = static_cast<double>(kb.c);
    }

    const double kh = static_cast<double>(kb.h);
    const double kl = static_cast<double>(kb.l);
    const double ko = static_cast<double>(kb.o);
    const double kc = static_cast<double>(kb.c);

    if (et >= 492 && et < 552) { st.pm_h = std::max(st.pm_h, kh); st.pm_l = std::min(st.pm_l, kl); st.has_pm = true; }
    if (et < 120) { st.asia_h = std::max(st.asia_h, kh); st.asia_l = std::min(st.asia_l, kl); st.has_asia = true; }
    if (et < 510) { st.on_h = std::max(st.on_h, kh); st.on_l = std::min(st.on_l, kl); st.has_on = true; }
    if (et >= 1080 && et < 1200) { st.init_h = std::max(st.init_h, kh); st.init_l = std::min(st.init_l, kl); st.has_init = true; }
    if (et >= 570 && et < 575) { st.c5_h = std::max(st.c5_h, kh); st.c5_l = std::min(st.c5_l, kl); st.has_c5 = true; }
    if (et >= 570 && et < 630) { st.h1_h = std::max(st.h1_h, kh); st.h1_l = std::min(st.h1_l, kl); st.has_h1 = true; }
    if (et >= 570 && et < 600) { st.r30_h = std::max(st.r30_h, kh); st.r30_l = std::min(st.r30_l, kl); st.has_r30 = true; }
    if (et >= 480 && et < 720) { st.c4_h = std::max(st.c4_h, kh); st.c4_l = std::min(st.c4_l, kl); st.has_c4 = true; }
    if (et >= 600 && et < 610) { st.m10_h = std::max(st.m10_h, kh); st.m10_l = std::min(st.m10_l, kl); st.has_m10 = true; }
    if (et >= 565 && et < 570) { st.pre_h = std::max(st.pre_h, kh); st.pre_l = std::min(st.pre_l, kl); st.has_pre = true; }
    if (et >= 570 && et < 585) { st.op15_h = std::max(st.op15_h, kh); st.op15_l = std::min(st.op15_l, kl); st.has_op15 = true; }
    if (et >= 840 && et < 870) { st.fomc_h = std::max(st.fomc_h, kh); st.fomc_l = std::min(st.fomc_l, kl); st.has_fomc = true; }
    if (et == 0) st.mid_open = ko;
    if (et == 570) st.ny_open = ko;
    if (et < 570) { st.on_sum += kc; st.on_cnt++; }

    st.vol_win.push(std::max(0.0, static_cast<double>(kb.v)));
}

inline u11::PluginSignal reels_on_bar(const u11::CausalBarView& view, void* state) {
    auto* st = static_cast<ReelsState*>(state);
    u11::PluginSignal sig{};

    const size_t cur_b = view.bar_index();
    int16_t cur_et = view.et_mins();
    int16_t prev_et = st->prev_et_min;

    if (st->last_seen_bar == std::numeric_limits<size_t>::max()) {
        size_t start_b = (cur_b > 720) ? (cur_b - 720) : 0;
        for (size_t k = start_b; k <= cur_b; ++k) {
            reels_step_bar(*st, view, -static_cast<int>(cur_b - k), cur_et);
        }
    } else if (cur_b > st->last_seen_bar) {
        size_t start_b = std::max(st->last_seen_bar + 1, (cur_b > 1440) ? (cur_b - 1440) : 0);
        for (size_t k = start_b; k <= cur_b; ++k) {
            prev_et = cur_et;
            reels_step_bar(*st, view, -static_cast<int>(cur_b - k), cur_et);
        }
    }
    st->last_seen_bar = cur_b;
    st->prev_et_min = cur_et;

    if (view.session_id() < 0 || view.in_position) return sig;
    if (st->day_trades_issued >= st->max_trades || view.sess_entries >= st->max_trades) return sig;
    if (cur_b < 5) return sig;

    const auto& b0 = view[0];
    const auto& b1 = view[-1];
    const auto& b2 = view[-2];
    const double o0 = static_cast<double>(b0.o), h0 = static_cast<double>(b0.h), l0 = static_cast<double>(b0.l), c0 = static_cast<double>(b0.c);
    const double h1 = static_cast<double>(b1.h), l1 = static_cast<double>(b1.l), c1 = static_cast<double>(b1.c);
    const double h2 = static_cast<double>(b2.h), l2 = static_cast<double>(b2.l);

    const double ir = inst_ratio(st->inst);
    const double scale = price_scale(o0, st->inst);
    const double pt_scale = ir * scale;
    const double vwap = (view.session_vwap(0) > 0.0) ? view.session_vwap(0) : c0;
    const double ema20 = view.ema20(0);
    const double pdh = view.has_prior_day() ? view.pdh() : (st->has_pd ? st->pdh : h0);
    const double pdl = view.has_prior_day() ? view.pdl() : (st->has_pd ? st->pdl : l0);
    const double pdc = view.has_prior_day() ? view.pdc() : (st->has_pd ? st->pdc : c0);

    int8_t dir = 0;
    double custom_sl_pts = st->sl_pts * pt_scale;
    const int rid = st->reel_id;
    const double sp = st->sig_param;

    auto crossed_min = [&](int target_m) -> bool {
        return (cur_et == target_m) || (prev_et >= 0 && prev_et < target_m && cur_et > target_m && cur_et <= target_m + 3);
    };

    if (rid == -1) {
        // kernel_reel_1: AustinVwap (09:31 ET close vs VWAP)
        if (crossed_min(571) && vwap > 0.0) {
            if (c0 > vwap) {
                dir = 1;
                custom_sl_pts = std::clamp(c0 - vwap, st->sl_pts * pt_scale, sp * pt_scale);
            } else if (c0 < vwap) {
                dir = -1;
                custom_sl_pts = std::clamp(vwap - c0, st->sl_pts * pt_scale, sp * pt_scale);
            }
        }
    } else if (rid == -2) {
        // kernel_reel_2: FomcGlitch (14:00-14:30 range, 14:30-15:30 sweep fade)
        if (st->has_fomc && cur_et >= 870 && cur_et < 930) {
            if (h0 > st->fomc_h) st->fomc_swept_h = true;
            if (l0 < st->fomc_l) st->fomc_swept_l = true;
            if (st->fomc_swept_h && c0 < st->fomc_h) {
                dir = -1;
                custom_sl_pts = std::clamp(h0 - c0 + 5.0 * pt_scale, st->sl_pts * pt_scale, sp * pt_scale);
                st->fomc_swept_h = false;
            } else if (st->fomc_swept_l && c0 > st->fomc_l) {
                dir = 1;
                custom_sl_pts = std::clamp(c0 - l0 + 5.0 * pt_scale, st->sl_pts * pt_scale, sp * pt_scale);
                st->fomc_swept_l = false;
            }
        }
    } else if (rid == -6) {
        // kernel_reel_6: HudsonC2C3 (15-bar window, C1 sweeps, C2 reverses, C3 expands)
        if (cur_b >= 15) {
            double prior_l = 1e18, prior_h = -1e18;
            for (int k = 3; k <= 14; ++k) {
                prior_l = std::min(prior_l, static_cast<double>(view[-k].l));
                prior_h = std::max(prior_h, static_cast<double>(view[-k].h));
            }
            bool bull_c = (l2 < prior_l) && (c1 > static_cast<double>(b1.o)) && (c1 > static_cast<double>(b2.c)) && (c0 > o0) && (c0 > h1);
            bool bear_c = (h2 > prior_h) && (c1 < static_cast<double>(b1.o)) && (c1 < static_cast<double>(b2.c)) && (c0 < o0) && (c0 < l1);
            if (bull_c) {
                dir = 1;
                custom_sl_pts = std::clamp(c0 - std::min(l1, l0), st->sl_pts * pt_scale, sp * pt_scale);
            } else if (bear_c) {
                dir = -1;
                custom_sl_pts = std::clamp(std::max(h1, h0) - c0, st->sl_pts * pt_scale, sp * pt_scale);
            }
        }
    } else if (rid == 1) {
        if (cur_et >= 570 && cur_et < 950) {
            double box = pdh - pdl;
            if (box >= 20.0 * pt_scale) {
                if (c0 >= pdh - sp * box && c0 < o0 && c0 < l1) dir = -1;
                else if (c0 <= pdl + sp * box && c0 > o0 && c0 > h1) dir = 1;
            }
        }
    } else if (rid == 2) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 20) {
            if (c0 > ema20 && c1 <= view.ema20(-1) && (h0 - l0) >= sp * pt_scale) dir = 1;
            else if (c0 < ema20 && c1 >= view.ema20(-1) && (h0 - l0) >= sp * pt_scale) dir = -1;
        }
    } else if (rid == 3) {
        if (cur_et >= 570 && cur_et < 630 && st->has_pm) {
            if (h0 > st->pm_h && c0 < st->pm_h) dir = -1;
            else if (l0 < st->pm_l && c0 > st->pm_l) dir = 1;
        }
    } else if (rid == 4) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 60) {
            if (c0 >= vwap + sp * pt_scale) dir = -1;
            else if (c0 <= vwap - sp * pt_scale) dir = 1;
        }
    } else if (rid == 5) {
        if (cur_et >= 630 && cur_et < 950 && st->has_h1) {
            if (c0 > st->h1_h && c1 <= st->h1_h) dir = 1;
            else if (c0 < st->h1_l && c1 >= st->h1_l) dir = -1;
        }
    } else if (rid == 6) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 5) {
            double ph = -1e18, pl = 1e18;
            for (int k = 1; k <= 4; ++k) { ph = std::max(ph, static_cast<double>(view[-k].h)); pl = std::min(pl, static_cast<double>(view[-k].l)); }
            if (h0 > ph && c0 < ph) dir = -1;
            else if (l0 < pl && c0 > pl) dir = 1;
        }
    } else if (rid == 7) {
        if (cur_et >= 600 && cur_et < 900 && st->has_r30) {
            if (c0 > st->r30_h && l0 <= ema20 && c0 > ema20) dir = 1;
            else if (c0 < st->r30_l && h0 >= ema20 && c0 < ema20) dir = -1;
        }
    } else if (rid == 8) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 21) {
            if (c0 > vwap && c0 > ema20 && c1 <= view.ema20(-1)) dir = 1;
            else if (c0 < vwap && c0 < ema20 && c1 >= view.ema20(-1)) dir = -1;
        }
    } else if (rid == 9) {
        if (crossed_min(574) && st->has_pd && st->has_prev_pd) {
            if (pdc > st->prev_pdh && c0 > h1) dir = 1;
            else if (pdc < st->prev_pdl && c0 < l1) dir = -1;
        }
    } else if (rid == 10) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 15) {
            if (c0 > pdh && (c0 - o0) >= sp * pt_scale) dir = 1;
            else if (c0 < pdl && (o0 - c0) >= sp * pt_scale) dir = -1;
        }
    } else if (rid == 11) {
        if (cur_et >= 510 && cur_et < 660 && st->has_asia) {
            if (h0 > st->asia_h && c0 < st->asia_h) dir = -1;
            else if (l0 < st->asia_l && c0 > st->asia_l) dir = 1;
        }
    } else if (rid == 12) {
        if (crossed_min(599) && (view.has_prior_day() || st->has_pd)) {
            if (c0 > pdh) dir = 1;
            else if (c0 < pdl) dir = -1;
        }
    } else if (rid == 13) {
        if (cur_et >= 570 && cur_et < 900) {
            if (l0 > h2 + sp * pt_scale && c0 > c1) dir = 1;
            else if (h0 < l2 - sp * pt_scale && c0 < c1) dir = -1;
        }
    } else if (rid == 14) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 55) {
            if (c0 > ema20 && l0 <= ema20 && c0 > o0) dir = 1;
            else if (c0 < ema20 && h0 >= ema20 && c0 < o0) dir = -1;
        }
    } else if (rid == 15) {
        if (cur_et >= 570 && cur_et < 660 && st->has_on) {
            if (h0 > st->on_h && c0 < st->on_h) dir = -1;
            else if (l0 < st->on_l && c0 > st->on_l) dir = 1;
        }
    } else if (rid == 16) {
        if (cur_et >= 570 && cur_et < 900) {
            double body = std::abs(c0 - o0);
            double rng = h0 - l0;
            if (rng >= sp * pt_scale) {
                double lw = std::min(o0, c0) - l0;
                double uw = h0 - std::max(o0, c0);
                if (lw >= 2.0 * body && uw <= 0.2 * rng) dir = 1;
                else if (uw >= 2.0 * body && lw <= 0.2 * rng) dir = -1;
            }
        }
    } else if (rid == 17) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 20) {
            double vavg = st->vol_win.mean();
            if (static_cast<double>(b0.v) >= sp * vavg && (h0 - l0) >= 12.0 * pt_scale) {
                dir = (c0 > o0) ? 1 : -1;
            }
        }
    } else if (rid == 18) {
        if (cur_et >= 575 && cur_et < 900 && st->ny_open > 0.0) {
            if (l0 <= st->ny_open && c0 > st->ny_open && o0 > st->ny_open) dir = 1;
            else if (h0 >= st->ny_open && c0 < st->ny_open && o0 < st->ny_open) dir = -1;
        }
    } else if (rid == 19) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 20) {
            int lb = std::clamp(static_cast<int>(std::round(sp)), 5, 40);
            double dh = -1e18, dl = 1e18;
            for (int k = 1; k <= lb; ++k) { dh = std::max(dh, static_cast<double>(view[-k].h)); dl = std::min(dl, static_cast<double>(view[-k].l)); }
            if (c0 > dh) dir = 1;
            else if (c0 < dl) dir = -1;
        }
    } else if (rid == 20) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 10) {
            double rh1 = static_cast<double>(view[-4].h), rh2 = static_cast<double>(view[-1].h);
            if (std::abs(rh1 - rh2) <= sp * pt_scale && h0 > std::max(rh1, rh2) && c0 < std::max(rh1, rh2)) dir = -1;
            double rl1 = static_cast<double>(view[-4].l), rl2 = static_cast<double>(view[-1].l);
            if (std::abs(rl1 - rl2) <= sp * pt_scale && l0 < std::min(rl1, rl2) && c0 > std::min(rl1, rl2)) dir = 1;
        }
    } else if (rid == 21) {
        if (cur_et >= 570 && cur_et < 900) {
            double fvg_low = h2, fvg_high = l0;
            if (fvg_high - fvg_low >= sp * pt_scale) {
                if (c0 < fvg_low && h0 >= fvg_low) dir = -1;
                else if (c0 > fvg_high && l0 <= fvg_high) dir = 1;
            }
        }
    } else if (rid == 22) {
        if (cur_et >= 570 && cur_et < 720 && st->has_asia) {
            if (l0 < st->asia_l && c0 > st->asia_l) dir = 1;
            else if (h0 > st->asia_h && c0 < st->asia_h) dir = -1;
        }
    } else if (rid == 23) {
        if (cur_et >= 570 && cur_et < 900 && (view.has_prior_day() || st->has_pd)) {
            if (h0 >= pdh && c0 < pdh && c0 < o0) dir = -1;
            else if (l0 <= pdl && c0 > pdl && c0 > o0) dir = 1;
        }
    } else if (rid == 24) {
        if (cur_et >= 1200 && cur_et < 1320 && st->has_init) {
            if (h0 > st->init_h && c0 < st->init_h) dir = -1;
            else if (l0 < st->init_l && c0 > st->init_l) dir = 1;
        }
    } else if (rid == 25) {
        if (cur_et >= 570 && cur_et < 900) {
            if (c0 > o0 && c1 < static_cast<double>(b1.o) && c0 > h1) dir = 1;
            else if (c0 < o0 && c1 > static_cast<double>(b1.o) && c0 < l1) dir = -1;
        }
    } else if (rid == 26) {
        if (cur_et >= 575 && cur_et < 660 && st->has_c5) {
            if (c0 > st->c5_h && c1 <= st->c5_h) dir = 1;
            else if (c0 < st->c5_l && c1 >= st->c5_l) dir = -1;
        }
    } else if (rid == 27) {
        if (cur_et >= 575 && cur_et < 660 && st->has_c5 && (view.has_prior_day() || st->has_pd)) {
            if (c0 > st->c5_h && c1 <= st->c5_h && c0 > pdh) dir = 1;
            else if (c0 < st->c5_l && c1 >= st->c5_l && c0 < pdl) dir = -1;
        }
    } else if (rid == 28) {
        if (cur_et >= 630 && cur_et < 950 && st->has_h1) {
            if (c0 > st->h1_h && c1 <= st->h1_h) dir = 1;
            else if (c0 < st->h1_l && c1 >= st->h1_l) dir = -1;
        }
    } else if (rid == 29) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 60) {
            double h60 = -1e18, l60 = 1e18;
            for (int k = 1; k <= 59; ++k) { h60 = std::max(h60, static_cast<double>(view[-k].h)); l60 = std::min(l60, static_cast<double>(view[-k].l)); }
            if (h0 > h60 && c0 < h60) dir = -1;
            else if (l0 < l60 && c0 > l60) dir = 1;
        }
    } else if (rid == 30) {
        if (cur_et >= 615 && cur_et < 690 && cur_b >= 20) {
            if (c0 > ema20 && l0 <= ema20 && c0 > o0) dir = 1;
            else if (c0 < ema20 && h0 >= ema20 && c0 < o0) dir = -1;
        }
    } else if (rid == 31) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 15) {
            double h15 = -1e18, l15 = 1e18;
            for (int k = 2; k <= 14; ++k) { h15 = std::max(h15, static_cast<double>(view[-k].h)); l15 = std::min(l15, static_cast<double>(view[-k].l)); }
            if (h1 > h15 && c0 < l1) dir = -1;
            else if (l1 < l15 && c0 > h1) dir = 1;
        }
    } else if (rid == 32) {
        if (cur_et >= 570 && cur_et < 900 && (view.has_prior_day() || st->has_pd)) {
            if (h1 >= pdh && c0 < l1 && c0 < o0) dir = -1;
            else if (l1 <= pdl && c0 > h1 && c0 > o0) dir = 1;
        }
    } else if (rid == 33) {
        if (cur_et >= 570 && cur_et < 585) {
            if (c0 > o0 && (c0 - o0) >= sp * pt_scale) dir = 1;
            else if (o0 > c0 && (o0 - c0) >= sp * pt_scale) dir = -1;
        }
    } else if (rid == 34) {
        if (cur_et >= 720 && cur_et < 950 && st->has_c4) {
            if (c0 > st->c4_h && c1 <= st->c4_h) dir = 1;
            else if (c0 < st->c4_l && c1 >= st->c4_l) dir = -1;
        }
    } else if (rid == 35) {
        if (cur_et >= 610 && cur_et < 660 && st->has_m10) {
            if (c0 > st->m10_h && c1 <= st->m10_h) dir = 1;
            else if (c0 < st->m10_l && c1 >= st->m10_l) dir = -1;
        }
    } else if (rid == 36) {
        if (cur_et >= 570 && cur_et < 630 && st->has_pre) {
            if (c0 > st->pre_h && c1 <= st->pre_h) dir = 1;
            else if (c0 < st->pre_l && c1 >= st->pre_l) dir = -1;
        }
    } else if (rid == 37) {
        if (cur_et >= 570 && cur_et < 900 && (view.has_prior_day() || st->has_pd)) {
            if (h0 > pdh && c0 < pdh) dir = -1;
            else if (l0 < pdl && c0 > pdl) dir = 1;
        }
    } else if (rid == 38) {
        if (cur_et >= 570 && st->on_cnt > 60) {
            double poc = st->on_sum / static_cast<double>(st->on_cnt);
            double off = sp * pt_scale;
            if (c0 > poc + off && c1 <= poc + off) dir = 1;
            else if (c0 < poc - off && c1 >= poc - off) dir = -1;
        }
    } else if (rid == 39) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 15) {
            double h15 = -1e18, l15 = 1e18;
            for (int k = 1; k <= 14; ++k) { h15 = std::max(h15, static_cast<double>(view[-k].h)); l15 = std::min(l15, static_cast<double>(view[-k].l)); }
            if (h0 > h15 && c0 < o0) dir = -1;
            else if (l0 < l15 && c0 > o0) dir = 1;
        }
    } else if (rid == 40) {
        if (cur_et >= 585 && cur_et < 660 && st->has_op15) {
            if (c0 > st->op15_h && c1 <= st->op15_h) dir = 1;
            else if (c0 < st->op15_l && c1 >= st->op15_l) dir = -1;
        }
    } else if (rid == 41) {
        if (cur_et >= 570 && cur_et < 630 && !st->found_fvg_today) {
            if (l0 > h2 + sp * pt_scale) { dir = 1; st->found_fvg_today = true; }
            else if (h0 < l2 - sp * pt_scale) { dir = -1; st->found_fvg_today = true; }
        }
    } else if (rid == 42) {
        if (cur_et >= 570 && cur_et < 900 && (view.has_prior_day() || st->has_pd)) {
            if (h0 >= pdh && (h0 - std::max(o0, c0)) >= sp * pt_scale) dir = -1;
            else if (l0 <= pdl && (std::min(o0, c0) - l0) >= sp * pt_scale) dir = 1;
        }
    } else if (rid == 43) {
        if (cur_et >= 600 && cur_et < 660) {
            if (l0 > h2 + sp * pt_scale && c0 > o0) dir = 1;
            else if (h0 < l2 - sp * pt_scale && c0 < o0) dir = -1;
        }
    } else if (rid == 44) {
        if (cur_et >= 570 && st->on_cnt > 60) {
            double poc = st->on_sum / static_cast<double>(st->on_cnt);
            double off = sp * pt_scale;
            if (c0 > poc + off && c1 <= poc + off) dir = 1;
            else if (c0 < poc - off && c1 >= poc - off) dir = -1;
        }
    } else if (rid == 45) {
        if (cur_et >= 610 && cur_et < 720 && st->has_m10) {
            if (c0 > st->m10_h && c1 <= st->m10_h) dir = 1;
            else if (c0 < st->m10_l && c1 >= st->m10_l) dir = -1;
        }
    } else if (rid == 46) {
        if (cur_et >= 570 && cur_et < 900 && cur_b >= 60) {
            double h60 = -1e18, l60 = 1e18;
            for (int k = 1; k <= 59; ++k) { h60 = std::max(h60, static_cast<double>(view[-k].h)); l60 = std::min(l60, static_cast<double>(view[-k].l)); }
            if (h0 > h60 && c0 < o0) dir = -1;
            else if (l0 < l60 && c0 > o0) dir = 1;
        }
    }

    if (dir != 0) {
        const double final_sl = std::max(st->inst.tick_size * 4.0, custom_sl_pts);
        sig.direction = dir;
        sig.order_type = 0;
        sig.sl_pts = final_sl;
        sig.tp_pts = st->rr_tp * final_sl;
        sig.be_trig_pts = (st->be_trig_r > 0.0) ? (st->be_trig_r * final_sl) : 0.0;
        sig.be_offset_pts = st->inst.tick_size;
        sig.ratchet_trig_pts = (st->ratchet_trig_r > 0.0) ? (st->ratchet_trig_r * final_sl) : 0.0;
        sig.ratchet_trail_pts = (st->ratchet_trail_r > 0.0) ? (st->ratchet_trail_r * final_sl) : 0.0;
        sig.max_hold_bars = 240;
        sig.cooldown_bars = 1;
        st->day_trades_issued++;
    }
    return sig;
}

// ============================================================================
// Descriptor Table + Compact Template Factory Wrappers (Fast Compile!)
// ============================================================================
struct Batch3EntryDesc {
    int id;
    int family; // 0=B6, 1=OrbV5, 2=B9, 3=B1011, 4=Reels
    const char* uid;
    const char* display_name;
    const char* super_family;
    const char* source_note;
    const char* fidelity;
    bool needs_cross_asset;
    int32_t session_mask;
    double p0, p1, p2, p3, p4;
    int i0, i1;
    double p5;
    int i2 = 0;
};

inline constexpr Batch3EntryDesc BATCH3_ENTRIES[154] = {
    { 116, 0, "phys_116", "#116 116_TDA_Persistence_Manifold_Collapse", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch6_20strats.cpp", "EXACT_SOURCE", false, 0xF, 1.2000, 0.8000, 4.0000, 1.5, 0.75, 12, 0, 0.0 },
    { 117, 0, "phys_117", "#117 117_Asymmetric_Copula_Tail_Dependence", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch6_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 2.0000, 0.9000, 4.2000, 1.5, 0.75, 12, 0, 0.0 },
    { 118, 0, "phys_118", "#118 118_HigherOrder_Markov_Transition_Matrix", "02_Physics_Quant_Multiverse_001_240", "118_HigherOrder_Markov_Transition_Matrix_core.cpp, 118_HigherOrder_Markov_Transition_Matrix_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.0000, 0.9000, 4.0000, 1.5, 0.75, 12, 0, 0.0 },
    { 119, 0, "phys_119", "#119 119_Volume_Clock_Flow_Acceleration", "02_Physics_Quant_Multiverse_001_240", "119_Volume_Clock_Flow_Acceleration_core.cpp, 119_Volume_Clock_Flow_Acceleration_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.8000, 0.9000, 4.5000, 1.5, 0.75, 12, 0, 0.0 },
    { 120, 0, "phys_120", "#120 120_Nonlinear_Volterra_Cross_Memory", "02_Physics_Quant_Multiverse_001_240", "120_Nonlinear_Volterra_Cross_Memory_core.cpp, 120_Nonlinear_Volterra_Cross_Memory_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.2000, 0.8500, 4.2000, 1.5, 0.75, 12, 0, 0.0 },
    { 141, 1, "phys_141", "#141 141_30m_Gibbs_Phase_Auction_Exp", "02_Physics_Quant_Multiverse_001_240", "141_30m_Gibbs_Phase_Auction_Exp_core.cpp, 141_30m_Gibbs_Phase_Auction_Exp_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, 0 },
    { 142, 1, "phys_142", "#142 142_30m_Kalman_Residual_Drift_Acc", "02_Physics_Quant_Multiverse_001_240", "142_30m_Kalman_Residual_Drift_Acc_core.cpp, 142_30m_Kalman_Residual_Drift_Acc_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.00, 0 },
    { 143, 1, "phys_143", "#143 143_30m_OU_Asymmetric_Jump_Escape", "02_Physics_Quant_Multiverse_001_240", "143_30m_OU_Asymmetric_Jump_Escape_core.cpp, 143_30m_OU_Asymmetric_Jump_Escape_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, -1 },
    { 144, 1, "phys_144", "#144 144_30m_Block_Absorption_Release", "02_Physics_Quant_Multiverse_001_240", "144_30m_Block_Absorption_Release_core.cpp, 144_30m_Block_Absorption_Release_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, 0 },
    { 145, 1, "phys_145", "#145 145_30m_Quantum_Potential_Tunneling", "02_Physics_Quant_Multiverse_001_240", "145_30m_Quantum_Potential_Tunneling_core.cpp, 145_30m_Quantum_Potential_Tunneling_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 2, 1.00, 0 },
    { 146, 1, "phys_146", "#146 146_30m_Volterra_Convexity_Kernel", "02_Physics_Quant_Multiverse_001_240", "146_30m_Volterra_Convexity_Kernel_core.cpp, 146_30m_Volterra_Convexity_Kernel_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 1, 1.00, 0 },
    { 147, 1, "phys_147", "#147 147_30m_MultiFractal_DFA_Shift", "02_Physics_Quant_Multiverse_001_240", "147_30m_MultiFractal_DFA_Shift_core.cpp, 147_30m_MultiFractal_DFA_Shift_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 6, 2, 1.00, 0 },
    { 148, 1, "phys_148", "#148 148_30m_EMD_Hilbert_Phase_Synchrony", "02_Physics_Quant_Multiverse_001_240", "148_30m_EMD_Hilbert_Phase_Synchrony_core.cpp, 148_30m_EMD_Hilbert_Phase_Synchrony_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.00, 0 },
    { 149, 1, "phys_149", "#149 149_30m_Volume_Clock_Rupture", "02_Physics_Quant_Multiverse_001_240", "149_30m_Volume_Clock_Rupture_core.cpp, 149_30m_Volume_Clock_Rupture_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.00, -1 },
    { 150, 1, "phys_150", "#150 150_30m_HigherOrder_Markov_Transition", "02_Physics_Quant_Multiverse_001_240", "150_30m_HigherOrder_Markov_Transition_core.cpp, 150_30m_HigherOrder_Markov_Transition_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 2, 1.00, 0 },
    { 151, 1, "phys_151", "#151 151_15m_Tsallis_Entropy_Impulse", "02_Physics_Quant_Multiverse_001_240", "151_15m_Tsallis_Entropy_Impulse_core.cpp, 151_15m_Tsallis_Entropy_Impulse_ground_truth.py", "EXACT_SOURCE", false, 0xF, 15.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, 0 },
    { 152, 1, "phys_152", "#152 152_15m_CrossAsset_Volatility_Tensor", "02_Physics_Quant_Multiverse_001_240", "152_15m_CrossAsset_Volatility_Tensor_core.cpp, 152_15m_CrossAsset_Volatility_Tensor_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 6, 1, 1.00, 0 },
    { 153, 1, "phys_153", "#153 153_15m_Hawkes_Intensity_Cascade", "02_Physics_Quant_Multiverse_001_240", "153_15m_Hawkes_Intensity_Cascade_core.cpp, 153_15m_Hawkes_Intensity_Cascade_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 1, 1.00, 0 },
    { 154, 1, "phys_154", "#154 154_15m_Lyapunov_Trajectory_Divergence", "02_Physics_Quant_Multiverse_001_240", "154_15m_Lyapunov_Trajectory_Divergence_core.cpp, 154_15m_Lyapunov_Trajectory_Divergence_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 6, 2, 1.00, 0 },
    { 155, 1, "phys_155", "#155 155_15m_Asymmetric_Copula_Surge", "02_Physics_Quant_Multiverse_001_240", "155_15m_Asymmetric_Copula_Surge_core.cpp, 155_15m_Asymmetric_Copula_Surge_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 2, 1.00, -1 },
    { 156, 1, "phys_156", "#156 156_60m_Structural_Dalton_Gibbs", "02_Physics_Quant_Multiverse_001_240", "156_60m_Structural_Dalton_Gibbs_core.cpp, 156_60m_Structural_Dalton_Gibbs_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 6, 1, 1.00, 0 },
    { 157, 1, "phys_157", "#157 157_60m_Kalman_Volterra_Equilibrium", "02_Physics_Quant_Multiverse_001_240", "157_60m_Kalman_Volterra_Equilibrium_core.cpp, 157_60m_Kalman_Volterra_Equilibrium_ground_truth.py", "EXACT_SOURCE", false, 0xF, 3.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, 0 },
    { 158, 1, "phys_158", "#158 158_60m_MultiScale_Kinetic_Dissipation", "02_Physics_Quant_Multiverse_001_240", "158_60m_MultiScale_Kinetic_Dissipation_core.cpp, 158_60m_MultiScale_Kinetic_Dissipation_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 4, 2, 1.00, 0 },
    { 159, 1, "phys_159", "#159 159_30m_OrderFlow_Hydraulic_Shock", "02_Physics_Quant_Multiverse_001_240", "159_30m_OrderFlow_Hydraulic_Shock_core.cpp, 159_30m_OrderFlow_Hydraulic_Shock_ground_truth.py", "EXACT_SOURCE", false, 0xF, 5.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, 0 },
    { 160, 1, "phys_160", "#160 160_30m_TDA_Persistence_Cycle_Collapse", "02_Physics_Quant_Multiverse_001_240", "160_30m_TDA_Persistence_Cycle_Collapse_core.cpp, 160_30m_TDA_Persistence_Cycle_Collapse_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 1, 1.00, -1 },
    { 161, 1, "phys_161", "#161 161_30m_Gibbs_Phase_Thermodynamic_Inv", "02_Physics_Quant_Multiverse_001_240", "161_30m_Gibbs_Phase_Thermodynamic_Inv_core.cpp, 161_30m_Gibbs_Phase_Thermodynamic_Inv_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, 0 },
    { 162, 1, "phys_162", "#162 162_30m_Kalman_Innovation_Vel_Inv", "02_Physics_Quant_Multiverse_001_240", "162_30m_Kalman_Innovation_Vel_Inv_core.cpp, 162_30m_Kalman_Innovation_Vel_Inv_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 10, 2, 1.00, 0 },
    { 163, 1, "phys_163", "#163 163_30m_OU_Asymmetric_Jump_Escape", "02_Physics_Quant_Multiverse_001_240", "163_30m_OU_Asymmetric_Jump_Escape_core.cpp, 163_30m_OU_Asymmetric_Jump_Escape_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.00, -1 },
    { 164, 1, "phys_164", "#164 164_30m_Microstructural_Block_Absorption", "02_Physics_Quant_Multiverse_001_240", "164_30m_Microstructural_Block_Absorption_core.cpp, 164_30m_Microstructural_Block_Absorption_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 14, 2, 1.00, 0 },
    { 165, 1, "phys_165", "#165 165_30m_Quantum_Potential_Tunnel_Inv", "02_Physics_Quant_Multiverse_001_240", "165_30m_Quantum_Potential_Tunnel_Inv_core.cpp, 165_30m_Quantum_Potential_Tunnel_Inv_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 2, 1.00, 0 },
    { 166, 1, "phys_166", "#166 166_30m_Volterra_Convexity_Kernel_Mem", "02_Physics_Quant_Multiverse_001_240", "166_30m_Volterra_Convexity_Kernel_Mem_core.cpp, 166_30m_Volterra_Convexity_Kernel_Mem_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 3, 1.00, 0 },
    { 167, 1, "phys_167", "#167 167_30m_MultiFractal_DFA_Hurst_Shift", "02_Physics_Quant_Multiverse_001_240", "167_30m_MultiFractal_DFA_Hurst_Shift_core.cpp, 167_30m_MultiFractal_DFA_Hurst_Shift_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 6, 2, 1.00, 0 },
    { 168, 1, "phys_168", "#168 168_30m_EMD_Hilbert_Phase_Synchrony", "02_Physics_Quant_Multiverse_001_240", "168_30m_EMD_Hilbert_Phase_Synchrony_core.cpp, 168_30m_EMD_Hilbert_Phase_Synchrony_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.00, 0 },
    { 169, 1, "phys_169", "#169 169_30m_Speed_Market_Volume_Clock", "02_Physics_Quant_Multiverse_001_240", "169_30m_Speed_Market_Volume_Clock_core.cpp, 169_30m_Speed_Market_Volume_Clock_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 10, 2, 1.00, -1 },
    { 170, 1, "phys_170", "#170 170_30m_HigherOrder_SemiMarkov_Inv", "02_Physics_Quant_Multiverse_001_240", "170_30m_HigherOrder_SemiMarkov_Inv_core.cpp, 170_30m_HigherOrder_SemiMarkov_Inv_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 4, 1.00, 0 },
    { 171, 1, "phys_171", "#171 171_30m_Tsallis_NonExtensive_Entropy_Surge", "02_Physics_Quant_Multiverse_001_240", "171_30m_Tsallis_NonExtensive_Entropy_Surge_core.cpp, 171_30m_Tsallis_NonExtensive_Entropy_Surge_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 10, 3, 1.00, 0 },
    { 172, 1, "phys_172", "#172 172_30m_CrossAsset_Covariance_Tensor", "02_Physics_Quant_Multiverse_001_240", "172_30m_CrossAsset_Covariance_Tensor_core.cpp, 172_30m_CrossAsset_Covariance_Tensor_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 3, 1.00, 0 },
    { 173, 1, "phys_173", "#173 173_30m_SelfExciting_Hawkes_Collapse", "02_Physics_Quant_Multiverse_001_240", "173_30m_SelfExciting_Hawkes_Collapse_core.cpp, 173_30m_SelfExciting_Hawkes_Collapse_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 10, 4, 1.00, 0 },
    { 174, 1, "phys_174", "#174 174_30m_FTLE_Lyapunov_Trajectory_Div", "02_Physics_Quant_Multiverse_001_240", "174_30m_FTLE_Lyapunov_Trajectory_Div_core.cpp, 174_30m_FTLE_Lyapunov_Trajectory_Div_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 16, 4, 1.00, 0 },
    { 175, 1, "phys_175", "#175 175_30m_Asymmetric_Copula_Tail_Surge", "02_Physics_Quant_Multiverse_001_240", "175_30m_Asymmetric_Copula_Tail_Surge_core.cpp, 175_30m_Asymmetric_Copula_Tail_Surge_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 8, 2, 1.00, -1 },
    { 176, 1, "phys_176", "#176 176_30m_Dalton_IB_Auction_Expansion", "02_Physics_Quant_Multiverse_001_240", "176_30m_Dalton_IB_Auction_Expansion_core.cpp, 176_30m_Dalton_IB_Auction_Expansion_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 14, 3, 1.00, 0 },
    { 177, 1, "phys_177", "#177 177_30m_Kalman_Volterra_NonMarkovian_Eq", "02_Physics_Quant_Multiverse_001_240", "177_30m_Kalman_Volterra_NonMarkovian_Eq_core.cpp, 177_30m_Kalman_Volterra_NonMarkovian_Eq_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 3, 1.00, -1 },
    { 178, 1, "phys_178", "#178 178_30m_MultiScale_Kinetic_Shockwave", "02_Physics_Quant_Multiverse_001_240", "178_30m_MultiScale_Kinetic_Shockwave_core.cpp, 178_30m_MultiScale_Kinetic_Shockwave_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.50, 1 },
    { 179, 1, "phys_179", "#179 179_30m_OrderFlow_Waterhammer_Pulse", "02_Physics_Quant_Multiverse_001_240", "179_30m_OrderFlow_Waterhammer_Pulse_core.cpp, 179_30m_OrderFlow_Waterhammer_Pulse_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 12, 2, 1.25, 0 },
    { 180, 1, "phys_180", "#180 180_30m_TDA_Persistence_Betti0_Collapse", "02_Physics_Quant_Multiverse_001_240", "180_30m_TDA_Persistence_Betti0_Collapse_core.cpp, 180_30m_TDA_Persistence_Betti0_Collapse_ground_truth.py", "EXACT_SOURCE", false, 0xF, 30.0, 36.0, 0.0, 0.50, 0.25, 14, 2, 1.25, 0 },
    { 181, 2, "phys_181", "#181 181_30m_Gibbs_Phase_Thermodynamic_Shock", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.2000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 182, 2, "phys_182", "#182 182_30m_Kalman_Innovation_Velocity_Surge", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.2000, 30.0, 7.00, 1.20, 0.30, 2, 0, 1.50 },
    { 183, 2, "phys_183", "#183 183_30m_Hawkes_CrossAsset_Jump_Cascade", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 184, 2, "phys_184", "#184 184_30m_Quantum_Potential_Tunnel_Inversion", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.4000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 185, 2, "phys_185", "#185 185_30m_Tsallis_NonExtensive_Entropy_Drop", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.4500, 30.0, 8.00, 1.15, 0.35, 2, 0, 1.50 },
    { 186, 2, "phys_186", "#186 186_30m_Volterra_Fractional_Memory_Decel", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.0000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 187, 2, "phys_187", "#187 187_30m_MultiFractal_DFA_Hurst_Flip", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.6500, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 188, 2, "phys_188", "#188 188_30m_EMD_Hilbert_Phase_Synchrony", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.4000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 189, 2, "phys_189", "#189 189_30m_FTLE_Lyapunov_Horizon_Rupture", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.1800, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 190, 2, "phys_190", "#190 190_30m_Volume_Clock_VPIN_Toxicity_Shock", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.6500, 30.0, 7.00, 1.15, 0.35, 2, 0, 1.50 },
    { 191, 2, "phys_191", "#191 191_30m_TDA_Persistence_Betti_Coalescence", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 192, 2, "phys_192", "#192 192_30m_Asymmetric_Copula_Tail_Surge", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.8000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 193, 2, "phys_193", "#193 193_30m_HigherOrder_SemiMarkov_Transition", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 194, 2, "phys_194", "#194 194_30m_OrderFlow_Hydraulic_Waterhammer", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 195, 2, "phys_195", "#195 195_30m_Rough_Fractional_Vol_Decoupling", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.3500, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 196, 2, "phys_196", "#196 196_30m_Spectral_Wavelet_CrossCoherence", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.2000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 197, 2, "phys_197", "#197 197_30m_Shannon_Transfer_Entropy_Spill", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.5000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 198, 2, "phys_198", "#198 198_30m_WignerVille_Energy_Concentration", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 3.5000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 199, 2, "phys_199", "#199 199_30m_LempelZiv_Algorithmic_Complexity", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 0.0000, 30.0, 7.00, 1.20, 0.25, 2, 0, 1.50 },
    { 200, 2, "phys_200", "#200 200_30m_Bifurcation_Feigenbaum_Cascade", "02_Physics_Quant_Multiverse_001_240", "multiverse_batch9_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF, 1.5000, 30.0, 7.00, 1.15, 0.30, 2, 0, 1.50 },
    { 201, 3, "phys_201", "#201 201_Generalized_Pareto_EVT_Tail_Exceedance", "02_Physics_Quant_Multiverse_001_240", "201_Generalized_Pareto_EVT_Tail_Exceedance_core.cpp, 201_Generalized_Pareto_EVT_Tail_Exceedance_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 4.00, 7.50, 1.10, 7.80, 2, 0, 2.0 },
    { 202, 3, "phys_202", "#202 202_Ornstein_Uhlenbeck_Drift_Breakdown", "02_Physics_Quant_Multiverse_001_240", "202_Ornstein_Uhlenbeck_Drift_Breakdown_core.cpp, 202_Ornstein_Uhlenbeck_Drift_Breakdown_ground_truth.py", "EXACT_SOURCE", false, 0xF, 0.9400, 4.20, 7.50, 1.30, 8.50, 2, 0, 2.0 },
    { 203, 3, "phys_203", "#203 203_Shannon_Mutual_Information_Flow", "02_Physics_Quant_Multiverse_001_240", "203_Shannon_Mutual_Information_Flow_core.cpp, 203_Shannon_Mutual_Information_Flow_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 4.00, 7.50, 1.10, 7.80, 2, 0, 2.0 },
    { 204, 3, "phys_204", "#204 204_Kalman_Filter_Innovation_Surge", "02_Physics_Quant_Multiverse_001_240", "204_Kalman_Filter_Innovation_Surge_core.cpp, 204_Kalman_Filter_Innovation_Surge_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.8000, 4.00, 7.50, 1.35, 8.00, 2, 0, 2.0 },
    { 205, 3, "phys_205", "#205 205_Microstructure_OFI_RunLength_Sweep", "02_Physics_Quant_Multiverse_001_240", "205_Microstructure_OFI_RunLength_Sweep_core.cpp, 205_Microstructure_OFI_RunLength_Sweep_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.2000, 4.00, 7.50, 1.25, 8.00, 2, 0, 2.0 },
    { 206, 3, "phys_206", "#206 206_Wavelet_Multiresolution_Energy", "02_Physics_Quant_Multiverse_001_240", "206_Wavelet_Multiresolution_Energy_core.cpp, 206_Wavelet_Multiresolution_Energy_ground_truth.py", "EXACT_SOURCE", false, 0xF, 0.8600, 4.00, 7.50, 2.00, 8.00, 2, 0, 2.0 },
    { 207, 3, "phys_207", "#207 207_Bessel_First_Exit_Time_Inversion", "02_Physics_Quant_Multiverse_001_240", "207_Bessel_First_Exit_Time_Inversion_core.cpp, 207_Bessel_First_Exit_Time_Inversion_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.8000, 4.20, 7.50, 1.30, 8.20, 2, 0, 2.0 },
    { 208, 3, "phys_208", "#208 208_Volume_Synchronized_VPIN_Toxicity", "02_Physics_Quant_Multiverse_001_240", "208_Volume_Synchronized_VPIN_Toxicity_core.cpp, 208_Volume_Synchronized_VPIN_Toxicity_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 0.4400, 4.00, 7.50, 1.40, 7.80, 2, 0, 2.0 },
    { 209, 3, "phys_209", "#209 209_Detrended_Fluctuation_Analysis_DFA", "02_Physics_Quant_Multiverse_001_240", "209_Detrended_Fluctuation_Analysis_DFA_core.cpp, 209_Detrended_Fluctuation_Analysis_DFA_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.0000, 4.00, 7.50, 1.10, 8.00, 2, 0, 2.0 },
    { 210, 3, "phys_210", "#210 210_Maximal_Lyapunov_Divergence_Shock", "02_Physics_Quant_Multiverse_001_240", "210_Maximal_Lyapunov_Divergence_Shock_core.cpp, 210_Maximal_Lyapunov_Divergence_Shock_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 0.4500, 4.00, 7.50, 1.30, 8.00, 2, 0, 2.0 },
    { 211, 3, "phys_211", "#211 211_Gini_Inequality_Volume_Concentration", "02_Physics_Quant_Multiverse_001_240", "211_Gini_Inequality_Volume_Concentration_core.cpp, 211_Gini_Inequality_Volume_Concentration_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 0.2500, 4.00, 7.50, 1.40, 7.80, 2, 0, 2.0 },
    { 212, 3, "phys_212", "#212 212_Cross_Asset_Beta_Volatility_Skew", "02_Physics_Quant_Multiverse_001_240", "212_Cross_Asset_Beta_Volatility_Skew_core.cpp, 212_Cross_Asset_Beta_Volatility_Skew_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.6500, 4.00, 7.50, 1.80, 8.00, 2, 0, 2.0 },
    { 213, 3, "phys_213", "#213 213_Stochastic_Resonance_Barrier_Rebound", "02_Physics_Quant_Multiverse_001_240", "213_Stochastic_Resonance_Barrier_Rebound_core.cpp, 213_Stochastic_Resonance_Barrier_Rebound_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.2000, 4.00, 7.50, 1.40, 8.00, 2, 0, 2.0 },
    { 214, 3, "phys_214", "#214 214_CVD_Absorption_Pullback_Reclaim", "02_Physics_Quant_Multiverse_001_240", "214_CVD_Absorption_Pullback_Reclaim_core.cpp, 214_CVD_Absorption_Pullback_Reclaim_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.2000, 4.00, 7.50, 1.25, 8.00, 2, 0, 2.0 },
    { 215, 3, "phys_215", "#215 215_Kuramoto_Phase_Synchronization", "02_Physics_Quant_Multiverse_001_240", "215_Kuramoto_Phase_Synchronization_core.cpp, 215_Kuramoto_Phase_Synchronization_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.4000, 4.00, 7.50, 1.20, 7.80, 2, 0, 2.0 },
    { 216, 3, "phys_216", "#216 216_Maxwell_Boltzmann_Kinetic_Escape", "02_Physics_Quant_Multiverse_001_240", "216_Maxwell_Boltzmann_Kinetic_Escape_core.cpp, 216_Maxwell_Boltzmann_Kinetic_Escape_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 0.5000, 4.00, 7.50, 1.50, 8.00, 2, 0, 2.0 },
    { 217, 3, "phys_217", "#217 217_Topological_Manifold_Collapse", "02_Physics_Quant_Multiverse_001_240", "217_Topological_Manifold_Collapse_core.cpp, 217_Topological_Manifold_Collapse_ground_truth.py", "EXACT_SOURCE", false, 0xF, 0.8200, 4.20, 7.50, 1.30, 8.20, 2, 0, 2.0 },
    { 218, 3, "phys_218", "#218 218_Fractional_Brownian_Motion_Memory", "02_Physics_Quant_Multiverse_001_240", "218_Fractional_Brownian_Motion_Memory_core.cpp, 218_Fractional_Brownian_Motion_Memory_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.8000, 4.00, 7.50, 1.00, 8.00, 2, 0, 2.0 },
    { 219, 3, "phys_219", "#219 219_Rare_Markov_Transition_Shock", "02_Physics_Quant_Multiverse_001_240", "219_Rare_Markov_Transition_Shock_core.cpp, 219_Rare_Markov_Transition_Shock_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.3000, 4.00, 7.50, 1.25, 8.00, 2, 0, 2.0 },
    { 220, 3, "phys_220", "#220 220_Fisher_Information_Geodesic_Surge", "02_Physics_Quant_Multiverse_001_240", "220_Fisher_Information_Geodesic_Surge_core.cpp, 220_Fisher_Information_Geodesic_Surge_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.4000, 4.00, 7.50, 1.25, 8.00, 2, 0, 2.0 },
    { 221, 3, "phys_221", "#221 221_Navier_Stokes_Reynolds_Laminar_Jet", "02_Physics_Quant_Multiverse_001_240", "221_Navier_Stokes_Reynolds_Laminar_Jet_core.cpp, 221_Navier_Stokes_Reynolds_Laminar_Jet_ground_truth.py", "EXACT_SOURCE", false, 0xF, 1.6000, 3.80, 20.00, 1.25, 7.00, 2, 0, 2.0 },
    { 222, 3, "phys_222", "#222 222_Minkowski_Spacetime_Cone_Escape", "02_Physics_Quant_Multiverse_001_240", "222_Minkowski_Spacetime_Cone_Escape_core.cpp, 222_Minkowski_Spacetime_Cone_Escape_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 0.3500, 3.80, 20.00, 1.30, 7.20, 2, 0, 2.0 },
    { 223, 3, "phys_223", "#223 223_Renormalization_Group_Fixed_Point", "02_Physics_Quant_Multiverse_001_240", "223_Renormalization_Group_Fixed_Point_core.cpp, 223_Renormalization_Group_Fixed_Point_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.6000, 4.00, 20.00, 1.60, 7.50, 2, 0, 2.0 },
    { 224, 3, "phys_224", "#224 224_Ergodic_Measure_Breaking_Anomaly", "02_Physics_Quant_Multiverse_001_240", "224_Ergodic_Measure_Breaking_Anomaly_core.cpp, 224_Ergodic_Measure_Breaking_Anomaly_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.2000, 3.80, 20.00, 1.30, 7.00, 2, 0, 2.0 },
    { 225, 3, "phys_225", "#225 225_Hawkes_Mutual_Excitation_Cascade", "02_Physics_Quant_Multiverse_001_240", "225_Hawkes_Mutual_Excitation_Cascade_core.cpp, 225_Hawkes_Mutual_Excitation_Cascade_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.8000, 3.80, 20.00, 1.10, 7.20, 2, 0, 2.0 },
    { 226, 3, "phys_226", "#226 226_Heston_Vol_of_Vol_Squeeze", "02_Physics_Quant_Multiverse_001_240", "226_Heston_Vol_of_Vol_Squeeze_core.cpp, 226_Heston_Vol_of_Vol_Squeeze_ground_truth.py", "EXACT_SOURCE", false, 0xF, 0.8000, 4.00, 20.00, 1.30, 7.50, 2, 0, 2.0 },
    { 227, 3, "phys_227", "#227 227_Graph_Network_Centrality_Breakout", "02_Physics_Quant_Multiverse_001_240", "227_Graph_Network_Centrality_Breakout_core.cpp, 227_Graph_Network_Centrality_Breakout_ground_truth.py", "EXACT_SOURCE", false, 0xF, 6.0000, 3.80, 20.00, 1.25, 7.00, 2, 0, 2.0 },
    { 228, 3, "phys_228", "#228 228_Soliton_Wave_Packet_Propagation", "02_Physics_Quant_Multiverse_001_240", "228_Soliton_Wave_Packet_Propagation_core.cpp, 228_Soliton_Wave_Packet_Propagation_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.2000, 3.80, 20.00, 1.20, 7.20, 2, 0, 2.0 },
    { 229, 3, "phys_229", "#229 229_Queuing_Theory_Server_Overflow", "02_Physics_Quant_Multiverse_001_240", "229_Queuing_Theory_Server_Overflow_core.cpp, 229_Queuing_Theory_Server_Overflow_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.2000, 3.80, 20.00, 1.50, 7.00, 2, 0, 2.0 },
    { 230, 3, "phys_230", "#230 230_Percolation_Critical_Highway", "02_Physics_Quant_Multiverse_001_240", "230_Percolation_Critical_Highway_core.cpp, 230_Percolation_Critical_Highway_ground_truth.py", "EXACT_SOURCE", false, 0xF, 4.0000, 4.00, 20.00, 2.20, 7.50, 2, 0, 2.0 },
    { 231, 3, "phys_231", "#231 231_KL_Relative_Entropy_Acceleration", "02_Physics_Quant_Multiverse_001_240", "231_KL_Relative_Entropy_Acceleration_core.cpp, 231_KL_Relative_Entropy_Acceleration_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.4000, 3.80, 20.00, 1.25, 7.20, 2, 0, 2.0 },
    { 232, 3, "phys_232", "#232 232_LZW_Compression_Deficit_Monotonicity", "02_Physics_Quant_Multiverse_001_240", "232_LZW_Compression_Deficit_Monotonicity_core.cpp, 232_LZW_Compression_Deficit_Monotonicity_ground_truth.py", "EXACT_SOURCE", false, 0xF, 4.0000, 3.80, 20.00, 1.20, 7.00, 2, 0, 2.0 },
    { 233, 3, "phys_233", "#233 233_Van_der_Pol_Relaxation_Rupture", "02_Physics_Quant_Multiverse_001_240", "233_Van_der_Pol_Relaxation_Rupture_core.cpp, 233_Van_der_Pol_Relaxation_Rupture_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.2000, 3.80, 20.00, 1.40, 7.00, 2, 0, 2.0 },
    { 234, 3, "phys_234", "#234 234_Wigner_Ville_Spectral_Singularity", "02_Physics_Quant_Multiverse_001_240", "234_Wigner_Ville_Spectral_Singularity_core.cpp, 234_Wigner_Ville_Spectral_Singularity_ground_truth.py", "EXACT_SOURCE", false, 0xF, 0.6500, 4.00, 20.00, 1.25, 7.50, 2, 0, 2.0 },
    { 235, 3, "phys_235", "#235 235_Pareto_Levy_Heavy_Tailed_Flight", "02_Physics_Quant_Multiverse_001_240", "235_Pareto_Levy_Heavy_Tailed_Flight_core.cpp, 235_Pareto_Levy_Heavy_Tailed_Flight_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.6000, 3.80, 20.00, 1.40, 7.00, 2, 0, 2.0 },
    { 236, 3, "phys_236", "#236 236_Ising_Ferromagnetic_Magnetization", "02_Physics_Quant_Multiverse_001_240", "236_Ising_Ferromagnetic_Magnetization_core.cpp, 236_Ising_Ferromagnetic_Magnetization_ground_truth.py", "EXACT_SOURCE", false, 0xF, 5.0000, 3.80, 20.00, 1.20, 7.00, 2, 0, 2.0 },
    { 237, 3, "phys_237", "#237 237_Fokker_Planck_Probability_Singularity", "02_Physics_Quant_Multiverse_001_240", "237_Fokker_Planck_Probability_Singularity_core.cpp, 237_Fokker_Planck_Probability_Singularity_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.4000, 3.80, 20.00, 1.50, 7.20, 2, 0, 2.0 },
    { 238, 3, "phys_238", "#238 238_Chebyshev_6th_Moment_Enclosure_Rupture", "02_Physics_Quant_Multiverse_001_240", "238_Chebyshev_6th_Moment_Enclosure_Rupture_core.cpp, 238_Chebyshev_6th_Moment_Enclosure_Rupture_ground_truth.py", "EXACT_SOURCE", false, 0xF, 3.2000, 3.80, 20.00, 1.30, 7.00, 2, 0, 2.0 },
    { 239, 3, "phys_239", "#239 239_CTRW_SuperDiffusive_Transport", "02_Physics_Quant_Multiverse_001_240", "239_CTRW_SuperDiffusive_Transport_core.cpp, 239_CTRW_SuperDiffusive_Transport_ground_truth.py", "EXACT_SOURCE", false, 0xF, 2.2000, 3.80, 20.00, 1.25, 7.20, 2, 0, 2.0 },
    { 240, 3, "phys_240", "#240 240_Symplectic_KAM_Resonant_Torus_Break", "02_Physics_Quant_Multiverse_001_240", "240_Symplectic_KAM_Resonant_Torus_Break_core.cpp, 240_Symplectic_KAM_Resonant_Torus_Break_ground_truth.py", "CROSS_ASSET_CAUSAL", true, 0xF, 1.2000, 4.00, 20.00, 1.35, 7.50, 2, 0, 2.0 },
    { -1, 4, "kernel_reel_1", "Austin McConnell 2nd Candle VWAP Breakout (Reel 1: DbtljC4tqD4)", "05_Instagram_Reels_46_Validation_Matrix", "austin_vwap.py, strategy_registry.py", "EXACT_SOURCE", false, 0x4, 30.0000, 10.00, 2.00, 0.00, 0.00, 1, 0, 0.00 },
    { -2, 4, "kernel_reel_2", "The FOMC Glitch Event Fade (Reel 2: DbZRqteo7z6)", "05_Instagram_Reels_46_Validation_Matrix", "fomc_glitch.py, strategy_registry.py", "EXACT_SOURCE", false, 0x8, 40.0000, 15.00, 2.00, 0.00, 0.00, 1, 0, 0.00 },
    { -6, 4, "kernel_reel_6", "Hudson Trades C2/C3 Continuation Model (Reel 6: DdrsUE2vPiq)", "05_Instagram_Reels_46_Validation_Matrix", "hudson_c2c3.py, strategy_registry.py", "EXACT_SOURCE", false, 0xc, 25.0000, 8.00, 5.00, 0.00, 0.00, 3, 0, 0.00 },
    { 1, 4, "reel46_01_box_theory_pdh_pdl_reversal", "Reel 01 (DcrYYMhyoQA): Box Theory (PDH/PDL Reversal)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 0.1500, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 2, 4, "reel46_02_adx_momentum_trend_filter", "Reel 02 (Ddnz7wPMuJo): ADX Momentum Trend Filter", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 8.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 3, 4, "reel46_03_premarket_8_12_9_12_am_sweep", "Reel 03 (DbBIlxUtXz0): Premarket 8:12-9:12 AM Sweep", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 570.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 4, 4, "reel46_04_ornstein_uhlenbeck_ou_mean_reversion", "Reel 04 (DdkVI-lti8o): Ornstein-Uhlenbeck (OU) Mean Reversion", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 35.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 5, 4, "reel46_05_john_wickwatcher_1h_orb_15m_breakout", "Reel 05 (DdTdQ-fALUR): John Wickwatcher 1H ORB + 15m Breakout", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 630.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 6, 4, "reel46_06_5_min_ebp_liquidity_sweep", "Reel 06 (Dd1iO9PgA6M): 5-Min EBP Liquidity Sweep", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 5.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 7, 4, "reel46_07_flexing_joe_30m_20_ema_pullback", "Reel 07 (Ddzb26MRUqs): Flexing Joe 30m 20-EMA Pullback", "05_Instagram_Reels_46_Validation_Matrix", "CATALOG_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_MATRIX.csv", "EXACT_SOURCE", false, 0xc, 600.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 8, 4, "reel46_08_vwap_9_21_ema_cross_continuation", "Reel 08 (Dd2Tt-KtlYP): VWAP + 9/21 EMA Cross Continuation", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 20.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 9, 4, "reel46_09_strat_c2_closure_into_continuation", "Reel 09 (Dd17W05T5aN): Strat C2 Closure into Continuation", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 575.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 10, 4, "reel46_10_4h_break_of_structure_bos_5m_displacement", "Reel 10 (DdypM0QtyFB): 4H Break of Structure (BOS) + 5M Displacement", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 10.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 11, 4, "reel46_11_ict_90_win_rate_killzone_model", "Reel 11 (DdjjN2CzmPX): ICT 90% Win Rate Killzone Model", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 120.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 12, 4, "reel46_12_4_hour_key_level_10_00_am_ny_continuation", "Reel 12 (Dd2IcmhTp-v): 4-Hour Key Level + 10:00 AM NY Continuation", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 600.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 13, 4, "reel46_13_htf_fvg_bias_lower_timeframe_entry", "Reel 13 (Dd1lYbPxMRk): HTF FVG Bias + Lower Timeframe Entry", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 6.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 14, 4, "reel46_14_multi_ema_ribbon_trend_expansion", "Reel 14 (Ddz14yWCKjH): Multi-EMA Ribbon Trend Expansion", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 20.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 15, 4, "reel46_15_skyyytrades_15m_asia_london_dual_liquidity_sweep", "Reel 15 (Dd2MzeJpBBS): SkyyyTrades 15m Asia & London Dual Liquidity Sweep", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 510.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 16, 4, "reel46_16_single_candle_rejection_hammer_shooting_star", "Reel 16 (DdjlUhogT6b): Single Candle Rejection (Hammer / Shooting Star)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 8.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 17, 4, "reel46_17_institutional_imbalance_volume_surge", "Reel 17 (DduXBv6yXFK): Institutional Imbalance Volume Surge", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 2.5000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 18, 4, "reel46_18_key_open_levels_retest_00_00_9_30_open", "Reel 18 (Dd1dRRWtV0D): Key Open Levels Retest (00:00 & 9:30 Open)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 575.0000, 20.0, 2.0, 1.00, 0.00, 2, 0, 0.00 },
    { 19, 4, "reel46_19_70_month_profitable_donchian_breakout", "Reel 19 (DdXJfSsTLlC): 70-Month Profitable Donchian Breakout", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 20.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 20, 4, "reel46_20_equal_highs_lows_eqh_eql_inducement_sweep", "Reel 20 (Dd4v2qryuBM): Equal Highs / Lows (EQH/EQL) Inducement Sweep", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 1.5000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 21, 4, "reel46_21_inversion_fvg_with_liquidity_buildup_lrlr", "Reel 21 (Dd4GJ3ZxrYw): Inversion FVG with Liquidity Buildup (LRLR)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 4.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 22, 4, "reel46_22_asian_sweep_reversal_expansion", "Reel 22 (Dd25i-hAThh): Asian Sweep Reversal Expansion", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 120.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 23, 4, "reel46_23_3_condition_alignment_htf_fvg_disp_entry", "Reel 23 (Dd197Nhy79j): 3-Condition Alignment (HTF FVG + Disp + Entry)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 1.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 24, 4, "reel46_24_asian_evening_payout_model_20_00_22_00", "Reel 24 (Dd5KSwUhB02): Asian Evening Payout Model (20:00-22:00)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x1, 1200.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 25, 4, "reel46_25_order_block_mitigation_momentum", "Reel 25 (Dd5T-2gMY0Q): Order Block Mitigation + Momentum", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 1.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 26, 4, "reel46_26_yostrades_9_30_opening_5m_candle_breakout", "Reel 26 (DdKJXBVvdZ7): YosTrades 9:30 Opening 5M Candle Breakout", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 575.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 27, 4, "reel46_27_pdh_pdl_breakout_expansion_model_model_d", "Reel 27 (DduchWAyFmI): PDH/PDL Breakout Expansion Model (Model D)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 575.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 28, 4, "reel46_28_mianomics_1st_hour_open_breakout", "Reel 28 (Dd2bY3_ARvJ): Mianomics 1st Hour Open Breakout", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 630.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 29, 4, "reel46_29_resting_orders_sweep_60_min_swing_sweep", "Reel 29 (Dd2RL9thwQT): Resting Orders Sweep (60-min swing sweep)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 60.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 30, 4, "reel46_30_post_open_momentum_trend_pullback", "Reel 30 (Dd6m4H6RWTP): Post-Open Momentum Trend Pullback", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 615.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 31, 4, "reel46_31_spanish_ict_sweep_choch_fvg", "Reel 31 (Dd1KEmjuL8n): Spanish ICT (Sweep -> CHoCH -> FVG)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 15.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 32, 4, "reel46_32_confirmation_entry_vs_blind_limit_entry", "Reel 32 (DduV68PMOIS): Confirmation Entry vs Blind Limit Entry", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 1.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 33, 4, "reel46_33_opening_drive_high_volume_momentum_scalp", "Reel 33 (Dd7dymYPVYZ): Opening Drive High Volume Momentum Scalp", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 15.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 34, 4, "reel46_34_first_4_hour_candle_model_08_00_12_00_breakout", "Reel 34 (Dd7JenOJLXk): First 4-Hour Candle Model (08:00-12:00 Breakout)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x8, 720.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 35, 4, "reel46_35_9am_central_10am_eastern_c_model_macro", "Reel 35 (DcpMVr_ODp5): 9AM Central / 10AM Eastern C-Model Macro", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 610.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 36, 4, "reel46_36_gstrades_5_min_pre_open_9_25_9_30_am_amd", "Reel 36 (Dd5awrTOiIa): GSTrades 5-Min Pre-Open (9:25-9:30 AM) AMD", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 570.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 37, 4, "reel46_37_internal_vs_external_liquidity_rebalancing", "Reel 37 (Dd6QlYARu7Q): Internal vs External Liquidity Rebalancing", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 1.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 38, 4, "reel46_38_amd_volume_profile_poc_vah_val", "Reel 38 (DduRi7So_TK): AMD + Volume Profile POC/VAH/VAL", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 20.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 39, 4, "reel46_39_confirmation_model_zero_red_days", "Reel 39 (DbUAGU9DH9z): Confirmation Model (Zero Red Days)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 15.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 40, 4, "reel46_40_nas100_9_30_sweep_into_9_45_continuation", "Reel 40 (Dd450SLoxr-): NAS100 9:30 Sweep into 9:45 Continuation", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 585.0000, 20.0, 2.0, 1.50, 0.00, 2, 0, 0.00 },
    { 41, 4, "reel46_41_first_presented_fair_value_gap_post_open", "Reel 41 (Dd7A7Vzt2qc): First Presented Fair Value Gap Post-Open", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 4.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 42, 4, "reel46_42_institutional_rejection_wicks_key_level_retest", "Reel 42 (Dd7hJzky3AR): Institutional Rejection Wicks Key Level Retest", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 10.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 43, 4, "reel46_43_kashfutures_sibe_model_silver_bullet_10_00_11_00_am", "Reel 43 (DcMXjDmRcHL): KashFutures SIBE Model (Silver Bullet 10:00-11:00 AM)", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 5.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 },
    { 44, 4, "reel46_44_overnight_volume_profile_poc_breakout", "Reel 44 (Dcyj-sGxXTv): Overnight Volume Profile POC Breakout", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 15.0000, 20.0, 2.0, 1.00, 0.00, 2, 0, 0.00 },
    { 45, 4, "reel46_45_loztradez_10_00_am_macro_expansion", "Reel 45 (Dd7lTdaIvd6): LozTradez 10:00 AM Macro Expansion", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0x4, 610.0000, 20.0, 2.0, 0.00, 0.00, 2, 0, 0.00 },
    { 46, 4, "reel46_46_4_hour_fractal_trendline_sweep", "Reel 46 (DZtWDMKRvlf): 4-Hour Fractal Trendline Sweep", "05_Instagram_Reels_46_Validation_Matrix", "FULL_46_REELS_VALIDATION_MATRIX.csv, FULL_46_REELS_VALIDATION_REPORT.md", "EXACT_SOURCE", false, 0xc, 60.0000, 20.0, 2.0, 0.00, 1.50, 2, 0, 0.75 }
};

template <int Idx>
inline void init_b6_idx(PhysB6State& st, const std::vector<double>& pv, const u11::InstrumentSpec* sp) {
    const auto& d = BATCH3_ENTRIES[Idx];
    init_b6_common(st, d.id, pv, sp, d.p0, d.p1, d.p2, d.i0);
}

template <int Idx>
inline void init_orbv5_idx(PhysOrbV5State& st, const std::vector<double>& pv, const u11::InstrumentSpec* sp) {
    const auto& d = BATCH3_ENTRIES[Idx];
    init_orbv5_common(st, d.id, pv, sp, static_cast<int>(d.p0), d.i2, d.i1, d.i0);
}

template <int Idx>
inline void init_b9_idx(PhysB9State& st, const std::vector<double>& pv, const u11::InstrumentSpec* sp) {
    const auto& d = BATCH3_ENTRIES[Idx];
    init_b9_common(st, d.id, pv, sp, d.p0, d.p2, d.p3, d.p4);
}

template <int Idx>
inline void init_b1011_idx(PhysB1011State& st, const std::vector<double>& pv, const u11::InstrumentSpec* sp) {
    const auto& d = BATCH3_ENTRIES[Idx];
    init_b1011_common(st, d.id, pv, sp, d.p0, d.p3, d.p1, d.p4, d.p2);
}

template <int Idx>
inline void init_reels_idx(ReelsState& st, const std::vector<double>& pv, const u11::InstrumentSpec* sp) {
    const auto& d = BATCH3_ENTRIES[Idx];
    init_reels_common(st, d.id, pv, sp, d.p0, d.p1, d.p2, d.p3, d.p4, d.p5, d.i0);
}

inline u11::ParamSchema build_schema_for_entry(const Batch3EntryDesc& d) {
    u11::ParamSchema sc;
    if (d.family == 0) {
        sc.params = {
            {"thresh",          d.p0, std::max(0.2, d.p0 * 0.5), d.p0 * 2.0, 5, u11::ParamScale::LINEAR, {}},
            {"sl_atr_mult",     d.p1, 0.4, 2.0, 5, u11::ParamScale::LINEAR, {}},
            {"tp_atr_mult",     d.p2, 1.5, 8.0, 5, u11::ParamScale::LINEAR, {}},
            {"ratchet_trig_r",  1.5,  0.0, 4.0, 5, u11::ParamScale::LINEAR, {}},
            {"ratchet_trail_r", 0.75, 0.2, 2.0, 4, u11::ParamScale::LINEAR, {}},
            {"max_entries",     static_cast<double>(d.i0), 1.0, 16.0, 4, u11::ParamScale::INT, {}}
        };
    } else if (d.family == 1) {
        sc.params = {
            {"or_mins",         d.p0, 3.0, 60.0, 6, u11::ParamScale::DISCRETE, {3.0, 5.0, 15.0, 30.0, 45.0, 60.0}},
            {"base_sl_pts",     36.0, 16.0, 72.0, 5, u11::ParamScale::LINEAR, {}},
            {"rr_tp",           0.0, 0.0, 6.0, 4, u11::ParamScale::LINEAR, {}},
            {"ratchet_trig_r",  0.50, 0.25, 2.0, 4, u11::ParamScale::LINEAR, {}},
            {"ratchet_trail_r", 0.25, 0.10, 1.0, 4, u11::ParamScale::LINEAR, {}},
            {"min_reentry",     static_cast<double>(d.i1), 1.0, 6.0, 6, u11::ParamScale::INT, {}},
            {"max_reentry",     static_cast<double>(d.i0), 2.0, 16.0, 5, u11::ParamScale::INT, {}}
        };
    } else if (d.family == 2) {
        double lo = (d.p0 > 0.0) ? d.p0 * 0.5 : 0.0;
        double hi = (d.p0 > 0.0) ? d.p0 * 2.0 : 2.0;
        sc.params = {
            {"or_mins",       30.0, 15.0, 60.0, 4, u11::ParamScale::DISCRETE, {15.0, 30.0, 45.0, 60.0}},
            {"signal_thresh", d.p0, lo, hi, 5, u11::ParamScale::LINEAR, {}},
            {"target_r",      d.p2, 2.0, 12.0, 5, u11::ParamScale::LINEAR, {}},
            {"be_trig_r",     d.p3, 0.5, 3.0, 4, u11::ParamScale::LINEAR, {}},
            {"be_lock_r",     d.p4, 0.0, 1.0, 4, u11::ParamScale::LINEAR, {}},
            {"max_entries",   2.0,  1.0, 6.0, 4, u11::ParamScale::INT, {}}
        };
    } else if (d.family == 3) {
        sc.params = {
            {"signal_thresh", d.p0, std::max(0.1, d.p0 * 0.5), d.p0 * 2.0, 5, u11::ParamScale::LINEAR, {}},
            {"vol_mult",      d.p3, 0.8, 2.5, 4, u11::ParamScale::LINEAR, {}},
            {"min_r_pts",     d.p1, 2.0, 8.0, 4, u11::ParamScale::LINEAR, {}},
            {"max_r_pts",     d.p4, 5.0, 15.0, 4, u11::ParamScale::LINEAR, {}},
            {"target_r",      d.p2, 2.0, 25.0, 5, u11::ParamScale::LINEAR, {}},
            {"max_entries",   2.0,  1.0, 6.0, 4, u11::ParamScale::INT, {}}
        };
    } else {
        sc.params = {
            {"sig_param",       d.p0, std::max(0.05, d.p0 * 0.5), d.p0 * 2.0, 5, u11::ParamScale::LINEAR, {}},
            {"sl_pts",          d.p1, 6.0, 60.0, 5, u11::ParamScale::LINEAR, {}},
            {"rr_tp",           d.p2, 0.75, 6.0, 5, u11::ParamScale::LINEAR, {}},
            {"be_trig_r",       d.p3, 0.0, 3.0, 4, u11::ParamScale::LINEAR, {}},
            {"ratchet_trig_r",  d.p4, 0.0, 4.0, 4, u11::ParamScale::LINEAR, {}},
            {"ratchet_trail_r", d.p5, 0.0, 2.0, 4, u11::ParamScale::LINEAR, {}},
            {"max_trades",      static_cast<double>(d.i0), 1.0, 6.0, 4, u11::ParamScale::INT, {}}
        };
    }
    return sc;
}

template <int Idx>
inline void push_batch3_entry(std::vector<s546::RegisteredStrategy>& out) {
    const auto& d = BATCH3_ENTRIES[Idx];
    s546::RegisteredStrategy rs;
    rs.uid = d.uid;
    rs.display_name = d.display_name;
    rs.super_family = d.super_family;
    rs.source_note = d.source_note;
    rs.fidelity = d.fidelity;
    rs.needs_cross_asset = d.needs_cross_asset;
    rs.session_mask = d.session_mask;
    rs.schema = build_schema_for_entry(d);
    if constexpr (BATCH3_ENTRIES[Idx].family == 0) {
        rs.factory = s546::make_factory<PhysB6State>(d.uid, phys_b6_on_bar, init_b6_idx<Idx>);
    } else if constexpr (BATCH3_ENTRIES[Idx].family == 1) {
        rs.factory = s546::make_factory<PhysOrbV5State>(d.uid, phys_orbv5_on_bar, init_orbv5_idx<Idx>);
    } else if constexpr (BATCH3_ENTRIES[Idx].family == 2) {
        rs.factory = s546::make_factory<PhysB9State>(d.uid, phys_b9_on_bar, init_b9_idx<Idx>);
    } else if constexpr (BATCH3_ENTRIES[Idx].family == 3) {
        rs.factory = s546::make_factory<PhysB1011State>(d.uid, phys_b1011_on_bar, init_b1011_idx<Idx>);
    } else {
        rs.factory = s546::make_factory<ReelsState>(d.uid, reels_on_bar, init_reels_idx<Idx>);
    }
    out.push_back(std::move(rs));
}

template <size_t... Is>
inline void register_batch3_seq(std::vector<s546::RegisteredStrategy>& out, std::index_sequence<Is...>) {
    (push_batch3_entry<static_cast<int>(Is)>(out), ...);
}

} // namespace b3_detail

inline void register_batch3(std::vector<s546::RegisteredStrategy>& out) {
    out.reserve(out.size() + 154);
    b3_detail::register_batch3_seq(out, std::make_index_sequence<154>{});
}

} // namespace s546
