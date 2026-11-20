#pragma once
#include "strategy_registry_546.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace s546 {
namespace b4 {

// ----------------------------------------------------------------------------
// Shared Causal & Symbol-Agnostic Helpers
// ----------------------------------------------------------------------------
inline double p_at(const std::vector<double>& p, size_t idx, double def_val) {
    return (idx < p.size()) ? p[idx] : def_val;
}

inline double sym_pt_scale(const u11::InstrumentSpec& inst) {
    // 1.0 on NQ (micro_pv=2.0), 0.4 on ES (micro_pv=5.0), 4.0 on YM (micro_pv=0.5)
    return 2.0 / std::max(0.1, inst.micro_pv);
}

inline double idx_scale(const u11::CausalBarView& view, const u11::InstrumentSpec& inst) {
    double px = static_cast<double>(view[0].o);
    return std::clamp(px / std::max(1.0, inst.baseline_index), 0.70, 3.00);
}

inline double rel_scale(const u11::CausalBarView& view, const u11::InstrumentSpec& inst) {
    return sym_pt_scale(inst) * idx_scale(view, inst);
}

inline double clamp_sl(double raw_sl, const u11::InstrumentSpec& inst) {
    return std::max(2.0 * inst.tick_size, raw_sl);
}

inline int weekday_et(int64_t ts_sec) {
    int64_t et_sec = ts_sec + u11::get_us_et_offset_sec(ts_sec);
    int64_t days = et_sec / 86400LL;
    if (et_sec < 0 && (et_sec % 86400LL) != 0) --days;
    // 1970-01-01 was Thursday (4); returns 0=Sun..6=Sat
    return static_cast<int>(((days + 4) % 7 + 7) % 7);
}

inline bool has_distinct_ref(const u11::CausalBarView& view, const u11::InstrumentSpec& inst) {
    if (inst.symbol != "ES" && view.has_ref_es()) return true;
    if (inst.symbol != "NQ" && view.has_ref_nq()) return true;
    if (inst.symbol != "YM" && view.has_ref_ym()) return true;
    return false;
}

inline const u11::RawBar& ref_bar(const u11::CausalBarView& view, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol != "ES" && view.has_ref_es()) return view.ref_es_bar(lag);
    if (inst.symbol != "NQ" && view.has_ref_nq()) return view.ref_nq_bar(lag);
    if (inst.symbol != "YM" && view.has_ref_ym()) return view.ref_ym_bar(lag);
    return view[lag];
}

inline double ref_atr14(const u11::CausalBarView& view, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol != "ES" && view.has_ref_es()) return view.ref_es_atr14(lag);
    if (inst.symbol != "NQ" && view.has_ref_nq()) return view.ref_nq_atr14(lag);
    if (inst.symbol != "YM" && view.has_ref_ym()) return view.ref_ym_atr14(lag);
    return view.atr14(lag);
}

inline double ref_ym_or_alt_bar_c(const u11::CausalBarView& view, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol != "YM" && view.has_ref_ym()) return static_cast<double>(view.ref_ym_bar(lag).c);
    if (inst.symbol != "ES" && view.has_ref_es()) return static_cast<double>(view.ref_es_bar(lag).c);
    if (inst.symbol != "NQ" && view.has_ref_nq()) return static_cast<double>(view.ref_nq_bar(lag).c);
    return static_cast<double>(view[lag].c);
}

inline double ref_ym_or_alt_atr(const u11::CausalBarView& view, const u11::InstrumentSpec& inst, int lag = 0) {
    if (inst.symbol != "YM" && view.has_ref_ym()) return view.ref_ym_atr14(lag);
    if (inst.symbol != "ES" && view.has_ref_es()) return view.ref_es_atr14(lag);
    if (inst.symbol != "NQ" && view.has_ref_nq()) return view.ref_nq_atr14(lag);
    return view.atr14(lag);
}

// Strictly causal backward scan within same CME day for ET time window [start_min, end_min)
inline bool scan_et_window_hl(
    const u11::CausalBarView& view,
    int start_min,
    int end_min,
    int max_back,
    double& hi_out,
    double& lo_out
) {
    hi_out = -1e18;
    lo_out = 1e18;
    int cnt = 0;
    int32_t cur_day_dt = view.day_yyyymmdd();
    int limit = static_cast<int>(std::min<size_t>(view.bar_index(), static_cast<size_t>(max_back)));
    for (int k = 0; k <= limit; ++k) {
        const auto& rb = view[-k];
        int16_t em = 0;
        int32_t dt = 0;
        u11::decode_et_and_cme_date(rb.ts_sec, em, dt);
        if (dt != cur_day_dt) break;
        if (em >= start_min && em < end_min) {
            hi_out = std::max(hi_out, static_cast<double>(rb.h));
            lo_out = std::min(lo_out, static_cast<double>(rb.l));
            ++cnt;
        }
    }
    return cnt >= 5 && hi_out > lo_out;
}

inline bool scan_et_anchor_close(
    const u11::CausalBarView& view,
    int target_min,
    int max_back,
    double& c_out
) {
    int32_t cur_day_dt = view.day_yyyymmdd();
    int limit = static_cast<int>(std::min<size_t>(view.bar_index(), static_cast<size_t>(max_back)));
    bool found = false;
    double best_c = 0.0;
    for (int k = limit; k >= 0; --k) {
        const auto& rb = view[-k];
        int16_t em = 0;
        int32_t dt = 0;
        u11::decode_et_and_cme_date(rb.ts_sec, em, dt);
        if (dt != cur_day_dt) continue;
        if (em >= target_min) {
            best_c = static_cast<double>(rb.c);
            found = true;
            break;
        }
    }
    c_out = best_c;
    return found;
}

inline bool scan_et_quantile_va(
    const u11::CausalBarView& view,
    int start_min,
    int end_min,
    int max_back,
    double& val_out,
    double& poc_out,
    double& vah_out
) {
    std::vector<double> prices;
    prices.reserve(256);
    int32_t cur_day_dt = view.day_yyyymmdd();
    int limit = static_cast<int>(std::min<size_t>(view.bar_index(), static_cast<size_t>(max_back)));
    for (int k = 1; k <= limit; ++k) {
        const auto& rb = view[-k];
        int16_t em = 0;
        int32_t dt = 0;
        u11::decode_et_and_cme_date(rb.ts_sec, em, dt);
        if (dt != cur_day_dt) break;
        if (em >= start_min && em < end_min) {
            prices.push_back(static_cast<double>(rb.c));
        }
    }
    if (prices.size() < 20) return false;
    std::sort(prices.begin(), prices.end());
    size_t n = prices.size();
    val_out = prices[static_cast<size_t>(0.15 * static_cast<double>(n))];
    poc_out = prices[static_cast<size_t>(0.50 * static_cast<double>(n))];
    vah_out = prices[static_cast<size_t>(0.85 * static_cast<double>(n))];
    return vah_out > val_out;
}

inline u11::PluginSignal make_sig(
    int8_t dir,
    int8_t otype,
    double trig,
    double sl_pts,
    double tp_pts,
    double r_trig,
    double r_trail,
    double be_trig,
    double be_off,
    int32_t max_hold,
    int32_t cd,
    const u11::InstrumentSpec& inst
) {
    u11::PluginSignal s{};
    if (dir == 0) return s;
    double sl = clamp_sl(sl_pts, inst);
    double tp = (tp_pts > 0.0) ? std::max(2.0 * inst.tick_size, tp_pts) : 0.0;
    s.direction = dir;
    s.order_type = otype;
    s.trigger_px = trig;
    s.sl_pts = sl;
    s.tp_pts = tp;
    s.ratchet_trig_pts = std::max(0.0, r_trig);
    s.ratchet_trail_pts = std::max(0.0, r_trail);
    s.be_trig_pts = std::max(0.0, be_trig);
    s.be_offset_pts = std::max(0.0, be_off);
    s.max_hold_bars = std::max(1, max_hold);
    s.cooldown_bars = std::max(0, cd);
    return s;
}

inline u11::ParamSpec p_lin(const std::string& n, double d, double lo, double hi, int gp = 5) {
    return u11::ParamSpec{n, d, lo, hi, gp, u11::ParamScale::LINEAR, {}};
}
inline u11::ParamSpec p_int(const std::string& n, double d, double lo, double hi, int gp = 5) {
    return u11::ParamSpec{n, d, lo, hi, gp, u11::ParamScale::INT, {}};
}
inline u11::ParamSpec p_bool(const std::string& n, double d) {
    return u11::ParamSpec{n, d, 0.0, 1.0, 2, u11::ParamScale::BOOL, {}};
}

// ============================================================================
// FAMILY 06: 06_Kernel_23_Reels_and_Creator_Adapters (32 Strategies)
// ============================================================================
enum F06Kind : int {
    F06_BB_FADE = 0,
    F06_CPP_UNIV,
    F06_R11_SHINOBI,
    F06_R11_FADE,
    F06_R12_ZENTRADES,
    F06_R12_TUNED,
    F06_R13_VEDACHALA,
    F06_R13_EXT,
    F06_R14_MONDAY,
    F06_R14_MDAY,
    F06_R15_MAFIA_3BAR,
    F06_R15_MAFIA_1M,
    F06_R16_RUMERS,
    F06_R16_FADE,
    F06_R1_AUSTIN_VWAP,
    F06_R2_FOMC_GLITCH,
    F06_R7_USMAN_ORB,
    F06_R10A_CIMI_30M,
    F06_R10B_CIMI_15M,
    F06_R3_BRANDON_IFVG,
    F06_R5_8AM_CANDLE,
    F06_R6_HUDSON_C2C3,
    F06_R8_SEBASTIAN_50EQ,
    F06_R9_ANTHONY_9PT,
    F06_VIBE_VWAP,
    F06_LIVE_R10A,
    F06_LIVE_R10B,
    F06_LIVE_R3,
    F06_LIVE_R4_MIDNIGHT,
    F06_LIVE_R5,
    F06_LIVE_R8,
    F06_LIVE_R9
};

struct F06State {
    F06Kind kind = F06_BB_FADE;
    u11::InstrumentSpec inst{};

    // Tunable parameters
    double p0 = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0;
    int max_trades_day = 3;
    int max_hold = 45;

    // Causal state
    RollingWindow rw_close{20};
    int32_t last_day = -1;
    int32_t last_sess = -1;
    int32_t trades_today = 0;

    // Intraday range / sweep state
    double r_hi = -1e18, r_lo = 1e18;
    bool r_ready = false;
    bool swept_hi = false, swept_lo = false;
    double sweep_hi_px = -1e18, sweep_lo_px = 1e18;

    // 1m OR & 3-bar acceptance
    double or1_h = -1e18, or1_l = 1e18;
    bool or1_ready = false;
    int consec_above = 0, consec_below = 0;

    // Prior-day / 2-day high/low
    double prev2_h = -1e18, prev2_l = 1e18;
    double prev1_h = -1e18, prev1_l = 1e18;
    bool has_2day = false;

    // 5m aggregator for Cimi / Usman / Hudson
    int cur_5m_bucket = -1;
    double b5_o = 0.0, b5_h = -1e18, b5_l = 1e18, b5_c = 0.0;
    bool b5_valid = false;
    int b5_count = 0;
    double c5_h[16]{}, c5_l[16]{}, c5_o[16]{}, c5_c[16]{};

    // Active FVG state (for Zentra / Brandon / Live Reels)
    double fvg_top = 0.0, fvg_bot = 0.0;
    int fvg_type = 0;
    bool fvg_active = false, fvg_inv = false;
};

template <F06Kind K>
inline void f06_init(F06State& st, const std::vector<double>& params, const u11::InstrumentSpec* spec) {
    st = F06State{};
    st.kind = K;
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    if constexpr (K == F06_BB_FADE) {
        int period = static_cast<int>(std::round(p_at(params, 0, 20.0)));
        st.rw_close.reset(static_cast<size_t>(std::max(5, period)));
        st.p0 = p_at(params, 1, 2.0);   // std_dev
        st.p1 = p_at(params, 2, 15.0);  // sl_pts
        st.p2 = p_at(params, 3, 15.0);  // tp_pts
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 20.0)));
    } else if constexpr (K == F06_CPP_UNIV) {
        st.p0 = p_at(params, 0, 30.0);  // or_mins
        st.p1 = p_at(params, 1, 1.50);  // hydraulic_th
        st.p2 = p_at(params, 2, 0.80);  // tsallis_cut
        st.p3 = p_at(params, 3, 3.00);  // volterra_cut
        st.p4 = p_at(params, 4, 36.25); // sl_pts
        st.p5 = p_at(params, 5, 60.0);  // tp_pts
        st.max_trades_day = 8;
        st.max_hold = 60;
    } else if constexpr (K == F06_R11_SHINOBI || K == F06_R11_FADE) {
        st.p0 = p_at(params, 0, 30.0);  // swing_lb
        st.p1 = p_at(params, 1, 0.618); // fib_lo
        st.p2 = p_at(params, 2, 0.786); // fib_hi
        st.p3 = p_at(params, 3, 18.0);  // sl_pts
        st.p4 = p_at(params, 4, 2.0);   // rr
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 5, 4.0)));
    } else if constexpr (K == F06_R12_ZENTRADES || K == F06_R12_TUNED) {
        double def_fvg = (K == F06_R12_TUNED) ? 4.0 : 1.5;
        double def_rr  = (K == F06_R12_TUNED) ? 3.0 : 2.0;
        double def_cap = (K == F06_R12_TUNED) ? 2.0 : 5.0;
        st.p0 = p_at(params, 0, def_fvg); // min_ifvg_pts
        st.p1 = p_at(params, 1, 15.0);    // sl_pts
        st.p2 = p_at(params, 2, def_rr);  // rr
        st.p3 = p_at(params, 3, 15.0);    // swing_lb
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, def_cap)));
    } else if constexpr (K == F06_R13_VEDACHALA || K == F06_R13_EXT) {
        double def_sl = (K == F06_R13_EXT) ? 25.0 : 15.0;
        double def_tp = (K == F06_R13_EXT) ? 80.0 : 25.0;
        st.p0 = p_at(params, 0, 20.0);   // poi_lb
        st.p1 = p_at(params, 1, def_sl); // sl_pts
        st.p2 = p_at(params, 2, def_tp); // tp_pts
        st.p3 = p_at(params, 3, 2.0);    // fvg_min
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 3.0)));
    } else if constexpr (K == F06_R14_MONDAY || K == F06_R14_MDAY) {
        st.p0 = p_at(params, 0, 15.0);  // sl_pts
        st.p1 = p_at(params, 1, 2.5);   // rr
        st.p2 = p_at(params, 2, 1.0);   // be_r
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 2.0)));
    } else if constexpr (K == F06_R15_MAFIA_3BAR || K == F06_R15_MAFIA_1M) {
        double def_consec = (K == F06_R15_MAFIA_3BAR) ? 3.0 : 1.0;
        st.p0 = p_at(params, 0, def_consec); // consec_bars
        st.p1 = p_at(params, 1, 20.0);       // sl_pts
        st.p2 = p_at(params, 2, 2.0);        // rr
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 2.0)));
    } else if constexpr (K == F06_R16_RUMERS || K == F06_R16_FADE) {
        st.p0 = p_at(params, 0, 0.15); // box_edge_frac
        st.p1 = p_at(params, 1, 20.0); // min_box_pts
        st.p2 = p_at(params, 2, 20.0); // sl_pts
        st.p3 = p_at(params, 3, 2.0);  // rr
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 3.0)));
    } else if constexpr (K == F06_R1_AUSTIN_VWAP) {
        st.p0 = p_at(params, 0, 10.0); // min_sl
        st.p1 = p_at(params, 1, 30.0); // max_sl
        st.p2 = p_at(params, 2, 2.0);  // rr
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 1.0)));
        st.max_hold = 90;
    } else if constexpr (K == F06_R2_FOMC_GLITCH) {
        st.p0 = p_at(params, 0, 15.0); // min_sl
        st.p1 = p_at(params, 1, 40.0); // max_sl
        st.p2 = p_at(params, 2, 2.0);  // rr
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 1.0)));
        st.max_hold = 60;
    } else if constexpr (K == F06_R7_USMAN_ORB) {
        st.p0 = p_at(params, 0, 15.0); // or_mins
        st.p1 = p_at(params, 1, 2.0);  // rr
        st.p2 = p_at(params, 2, 40.0); // max_sl_pts
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 1.0)));
        st.max_hold = 120;
    } else if constexpr (K == F06_R10A_CIMI_30M || K == F06_R10B_CIMI_15M || K == F06_LIVE_R10A || K == F06_LIVE_R10B) {
        bool is_30 = (K == F06_R10A_CIMI_30M || K == F06_LIVE_R10A);
        st.p0 = p_at(params, 0, is_30 ? 30.0 : 15.0);
        st.p1 = p_at(params, 1, is_30 ? 1.0 : 0.5);
        st.p2 = p_at(params, 2, (K == F06_LIVE_R10A) ? 10.0 : ((K == F06_LIVE_R10B) ? 8.0 : 0.0));
        st.p3 = p_at(params, 3, is_30 ? 25.0 : 20.0);
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 1.0)));
        st.max_hold = 90;
    } else if constexpr (K == F06_R3_BRANDON_IFVG || K == F06_R8_SEBASTIAN_50EQ || K == F06_R9_ANTHONY_9PT ||
                         K == F06_LIVE_R3 || K == F06_LIVE_R8 || K == F06_LIVE_R9) {
        double def_gap = (K == F06_R9_ANTHONY_9PT || K == F06_LIVE_R9) ? 9.0 :
                         ((K == F06_LIVE_R3 || K == F06_LIVE_R8) ? 4.0 : 5.0);
        st.p0 = p_at(params, 0, def_gap); // min_fvg_pts
        st.p1 = p_at(params, 1, 2.0);     // rr
        st.p2 = p_at(params, 2, 15.0);    // sl_pts fallback
        st.p3 = p_at(params, 3, 10.0);    // swing_lb
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 2.0)));
    } else if constexpr (K == F06_R5_8AM_CANDLE || K == F06_LIVE_R5) {
        st.p0 = p_at(params, 0, (K == F06_LIVE_R5) ? 6.0 : 2.0); // buffer/min_risk
        st.p1 = p_at(params, 1, 18.0);                           // sl_pts
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 2, 1.0)));
        st.max_hold = 90;
    } else if constexpr (K == F06_R6_HUDSON_C2C3) {
        st.p0 = p_at(params, 0, 5.0);  // rr
        st.p1 = p_at(params, 1, 8.0);  // min_sl
        st.p2 = p_at(params, 2, 25.0); // max_sl
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 3.0)));
    } else if constexpr (K == F06_VIBE_VWAP) {
        st.p0 = p_at(params, 0, 15.0); // threshold_pts
        st.p1 = p_at(params, 1, 10.0); // sl_pts
        st.p2 = p_at(params, 2, 15.0); // tp_pts
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 6.0)));
    } else if constexpr (K == F06_LIVE_R4_MIDNIGHT) {
        st.p0 = p_at(params, 0, 8.0);  // buffer_pts
        st.p1 = p_at(params, 1, 20.0); // sl_pts
        st.p2 = p_at(params, 2, 25.0); // tp_pts
        st.max_trades_day = static_cast<int>(std::round(p_at(params, 3, 2.0)));
        st.max_hold = 120;
    }
}

inline u11::PluginSignal f06_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto& st = *static_cast<F06State*>(state_ptr);
    u11::PluginSignal sig{};

    const auto& b0 = view[0];
    double c = static_cast<double>(b0.c);
    double h = static_cast<double>(b0.h);
    double l = static_cast<double>(b0.l);
    double o = static_cast<double>(b0.o);
    double v = std::max(1.0, static_cast<double>(b0.v));
    double sc = sym_pt_scale(st.inst);
    double atr = std::max(st.inst.tick_size * 2.0, view.atr14(0));
    int16_t em = view.et_mins();
    int32_t d = view.cme_day_idx();
    int8_t sess = view.session_id();

    if (d != st.last_day) {
        if (st.last_day >= 0 && view.has_prior_day()) {
            st.prev2_h = st.prev1_h;
            st.prev2_l = st.prev1_l;
            st.prev1_h = view.pdh();
            st.prev1_l = view.pdl();
            st.has_2day = (st.prev2_h > st.prev2_l && st.prev1_h > st.prev1_l);
        }
        st.last_day = d;
        st.trades_today = 0;
        st.r_hi = -1e18; st.r_lo = 1e18; st.r_ready = false;
        st.swept_hi = false; st.swept_lo = false;
        st.sweep_hi_px = -1e18; st.sweep_lo_px = 1e18;
        st.fvg_active = false; st.fvg_inv = false;
        st.cur_5m_bucket = -1; st.b5_valid = false; st.b5_count = 0;
    }
    if (sess != st.last_sess || view.is_new_sess()) {
        st.last_sess = sess;
        st.or1_h = h; st.or1_l = l; st.or1_ready = false;
        st.consec_above = 0; st.consec_below = 0;
    }
    if (view.min_in_sess() >= 1) {
        st.or1_ready = true;
    }

    // Update rolling close window & 5m bar aggregator causally
    st.rw_close.push(c);
    int b5 = (em / 5);
    bool b5_just_closed = false;
    if (b5 != st.cur_5m_bucket) {
        if (st.b5_valid) {
            for (int k = 15; k > 0; --k) {
                st.c5_o[k] = st.c5_o[k - 1];
                st.c5_h[k] = st.c5_h[k - 1];
                st.c5_l[k] = st.c5_l[k - 1];
                st.c5_c[k] = st.c5_c[k - 1];
            }
            st.c5_o[0] = st.b5_o; st.c5_h[0] = st.b5_h; st.c5_l[0] = st.b5_l; st.c5_c[0] = st.b5_c;
            st.b5_count++;
            b5_just_closed = true;
        }
        st.cur_5m_bucket = b5;
        st.b5_o = o; st.b5_h = h; st.b5_l = l; st.b5_c = c;
        st.b5_valid = true;
    } else {
        st.b5_h = std::max(st.b5_h, h);
        st.b5_l = std::min(st.b5_l, l);
        st.b5_c = c;
    }

    // FOMC glitch range update (14:00-14:30 ET) even while in_position
    if (st.kind == F06_R2_FOMC_GLITCH) {
        if (em >= 840 && em < 870) {
            st.r_hi = std::max(st.r_hi, h);
            st.r_lo = std::min(st.r_lo, l);
        } else if (em >= 870 && !st.r_ready && st.r_hi > st.r_lo) {
            st.r_ready = true;
        }
    } else if (st.kind == F06_LIVE_R4_MIDNIGHT) {
        if (em >= 1140 && em < 1200) {
            st.r_hi = std::max(st.r_hi, h);
            st.r_lo = std::min(st.r_lo, l);
        } else if ((em >= 1200 || em < 360) && !st.r_ready && st.r_hi > st.r_lo) {
            st.r_ready = true;
        }
    }

    if (view.bar_index() < 25 || view.in_position) return sig;
    if (st.trades_today >= st.max_trades_day) return sig;

    switch (st.kind) {
        case F06_BB_FADE: {
            if (!st.rw_close.full()) return sig;
            double ma = st.rw_close.mean();
            double sd = st.rw_close.stdev();
            if (sd <= 0.0) return sig;
            double up = ma + st.p0 * sd;
            double dn = ma - st.p0 * sd;
            double sl = st.p1 * sc;
            double tp = st.p2 * sc;
            if (c <= dn) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, 30, 1, st.inst);
            }
            if (c >= up) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, 30, 1, st.inst);
            }
            return sig;
        }
        case F06_CPP_UNIV: {
            int or_m = static_cast<int>(std::round(st.p0));
            if (or_m <= 15) or_m = 15;
            else if (or_m <= 30) or_m = 30;
            else if (or_m <= 45) or_m = 45;
            else or_m = 60;
            if (!view.or_formed(or_m) || view.min_in_sess() < or_m) return sig;
            double oh = view.or_high(or_m), ol = view.or_low(or_m);
            double c1 = static_cast<double>(view[-1].c);
            double c2 = static_cast<double>(view[-2].c);
            double pulse = (c - c2) * v / ((atr + 1e-4) * 1000.0);
            double p1 = std::abs(c - c1) / (atr + 1e-4) + 1e-4;
            double p2 = std::abs(c1 - c2) / (atr + 1e-4) + 1e-4;
            double nm = p1 + p2;
            p1 /= nm; p2 /= nm;
            double tsallis = (1.0 - (std::pow(p1, 1.5) + std::pow(p2, 1.5))) / 0.5;
            double volt = 0.0;
            for (int k = 0; k < 8; ++k) {
                volt += std::pow(static_cast<double>(k + 1), -0.4) * (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
            }
            double sl = st.p4 * sc, tp = st.p5 * sc;
            if (c > oh && (pulse >= st.p1 || (tsallis <= st.p2 && c > c1) || volt >= st.p3)) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 2.5 * sc, 1.25 * sc, 0, 0, st.max_hold, 2, st.inst);
            }
            if (c < ol && (pulse <= -st.p1 || (tsallis <= st.p2 && c < c1) || volt <= -st.p3)) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 2.5 * sc, 1.25 * sc, 0, 0, st.max_hold, 2, st.inst);
            }
            return sig;
        }
        case F06_R11_SHINOBI:
        case F06_R11_FADE: {
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 10, 80);
            double sw_hi = -1e18, sw_lo = 1e18;
            int hi_k = 0, lo_k = 0;
            for (int k = 1; k <= lb; ++k) {
                if (static_cast<double>(view[-k].h) > sw_hi) { sw_hi = static_cast<double>(view[-k].h); hi_k = k; }
                if (static_cast<double>(view[-k].l) < sw_lo) { sw_lo = static_cast<double>(view[-k].l); lo_k = k; }
            }
            double rng = sw_hi - sw_lo;
            if (rng < 5.0 * sc) return sig;
            int8_t raw_dir = 0;
            // Upswing (lo_k > hi_k) -> OTE pullback into 0.618..0.786 retracement from high
            if (lo_k > hi_k) {
                double retrace = (sw_hi - c) / rng;
                if (retrace >= st.p1 && retrace <= st.p2 && c > o) raw_dir = 1;
            } else if (hi_k > lo_k) {
                double retrace = (c - sw_lo) / rng;
                if (retrace >= st.p1 && retrace <= st.p2 && c < o) raw_dir = -1;
            }
            if (raw_dir == 0) return sig;
            int8_t dir = (st.kind == F06_R11_FADE) ? static_cast<int8_t>(-raw_dir) : raw_dir;
            double sl = st.p3 * sc;
            st.trades_today++;
            return make_sig(dir, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, 45, 3, st.inst);
        }
        case F06_R12_ZENTRADES:
        case F06_R12_TUNED: {
            if (!view.or_formed(15)) return sig;
            int lb = std::clamp(static_cast<int>(std::round(st.p3)), 5, 40);
            double p_lo = 1e18, p_hi = -1e18;
            for (int k = 3; k <= lb + 2; ++k) {
                p_lo = std::min(p_lo, static_cast<double>(view[-k].l));
                p_hi = std::max(p_hi, static_cast<double>(view[-k].h));
            }
            double min_gap = st.p0 * sc;
            double b1_h = static_cast<double>(view[-2].h), b1_l = static_cast<double>(view[-2].l);
            if (l > b1_h && (l - b1_h) >= min_gap) {
                st.fvg_top = l; st.fvg_bot = b1_h; st.fvg_type = 1; st.fvg_active = true; st.fvg_inv = false;
            } else if (b1_l > h && (b1_l - h) >= min_gap) {
                st.fvg_top = b1_l; st.fvg_bot = h; st.fvg_type = -1; st.fvg_active = true; st.fvg_inv = false;
            }
            if (st.fvg_active && !st.fvg_inv) {
                if (st.fvg_type == 1 && c < st.fvg_bot) st.fvg_inv = true;
                if (st.fvg_type == -1 && c > st.fvg_top) st.fvg_inv = true;
            }
            double sl = st.p1 * sc;
            if (st.fvg_active && st.fvg_inv) {
                if (st.fvg_type == -1 && static_cast<double>(view[-2].l) < p_lo && c > st.fvg_top) {
                    st.fvg_active = false; st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, 45, 3, st.inst);
                }
                if (st.fvg_type == 1 && static_cast<double>(view[-2].h) > p_hi && c < st.fvg_bot) {
                    st.fvg_active = false; st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, 45, 3, st.inst);
                }
            }
            return sig;
        }
        case F06_R13_VEDACHALA:
        case F06_R13_EXT: {
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 8, 60);
            double min_g = st.p3 * sc;
            // FVG POI from bars -3..-1
            double b3_h = static_cast<double>(view[-3].h), b3_l = static_cast<double>(view[-3].l);
            double b1_h = static_cast<double>(view[-1].h), b1_l = static_cast<double>(view[-1].l);
            if (b1_l > b3_h + min_g) {
                st.fvg_top = b1_l; st.fvg_bot = b3_h; st.fvg_type = 1; st.fvg_active = true;
            } else if (b3_l > b1_h + min_g) {
                st.fvg_top = b3_l; st.fvg_bot = b1_h; st.fvg_type = -1; st.fvg_active = true;
            }
            double hi = -1e18, lo = 1e18;
            for (int k = 1; k <= lb; ++k) {
                hi = std::max(hi, static_cast<double>(view[-k].h));
                lo = std::min(lo, static_cast<double>(view[-k].l));
            }
            double sl = st.p1 * sc, tp = st.p2 * sc;
            if (st.fvg_active && st.fvg_type == 1 && l <= st.fvg_top && c > st.fvg_bot && c > view.ema20(0)) {
                st.fvg_active = false; st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, 60, 2, st.inst);
            }
            if (st.fvg_active && st.fvg_type == -1 && h >= st.fvg_bot && c < st.fvg_top && c < view.ema20(0)) {
                st.fvg_active = false; st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, 60, 2, st.inst);
            }
            (void)hi; (void)lo;
            return sig;
        }
        case F06_R14_MONDAY: {
            if (weekday_et(b0.ts_sec) != 1 || !view.has_prior_day()) return sig;
            double pdh = view.pdh(), pdl = view.pdl();
            double sl = st.p0 * sc;
            if (static_cast<double>(view[-1].l) < pdl && c > pdl && c > o) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, 60, 5, st.inst);
            }
            if (static_cast<double>(view[-1].h) > pdh && c < pdh && c < o) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, 60, 5, st.inst);
            }
            return sig;
        }
        case F06_R14_MDAY: {
            if (!st.has_2day) return sig;
            double h2 = std::max(st.prev1_h, st.prev2_h);
            double l2 = std::min(st.prev1_l, st.prev2_l);
            double sl = st.p0 * sc;
            double be = st.p2 * sl;
            if (static_cast<double>(view[-1].l) < l2 && c > l2 && c > o) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p1, 0, 0, be, 0.25 * sc, 90, 5, st.inst);
            }
            if (static_cast<double>(view[-1].h) > h2 && c < h2 && c < o) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p1, 0, 0, be, 0.25 * sc, 90, 5, st.inst);
            }
            return sig;
        }
        case F06_R15_MAFIA_3BAR:
        case F06_R15_MAFIA_1M: {
            int req_bars = std::max(1, static_cast<int>(std::round(st.p0)));
            double oh = 0.0, ol = 0.0;
            if (st.kind == F06_R15_MAFIA_1M) {
                if (!st.or1_ready || st.or1_h <= st.or1_l) return sig;
                oh = st.or1_h; ol = st.or1_l;
            } else {
                if (!view.or_formed(15)) return sig;
                oh = view.or_high(15); ol = view.or_low(15);
            }
            if (c > oh) { st.consec_above++; st.consec_below = 0; }
            else if (c < ol) { st.consec_below++; st.consec_above = 0; }
            else { st.consec_above = 0; st.consec_below = 0; }
            double sl = st.p1 * sc;
            if (st.consec_above == req_bars) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, 60, 2, st.inst);
            }
            if (st.consec_below == req_bars) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, 60, 2, st.inst);
            }
            return sig;
        }
        case F06_R16_RUMERS:
        case F06_R16_FADE: {
            if (!view.has_prior_day()) return sig;
            double pdh = view.pdh(), pdl = view.pdl();
            double box_rng = pdh - pdl;
            if (box_rng < st.p1 * sc) return sig;
            double c1 = static_cast<double>(view[-1].c);
            double o1 = static_cast<double>(view[-1].o);
            double h2 = static_cast<double>(view[-2].h);
            double l2 = static_cast<double>(view[-2].l);
            int8_t raw_dir = 0;
            if (c1 >= pdh - st.p0 * box_rng && c1 < o1 && c1 < l2) raw_dir = -1;
            else if (c1 <= pdl + st.p0 * box_rng && c1 > o1 && c1 > h2) raw_dir = 1;
            if (raw_dir == 0) return sig;
            int8_t dir = (st.kind == F06_R16_FADE) ? static_cast<int8_t>(-raw_dir) : raw_dir;
            double sl = st.p2 * sc;
            st.trades_today++;
            return make_sig(dir, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, 45, 3, st.inst);
        }
        case F06_R1_AUSTIN_VWAP: {
            if (em != 571) return sig;
            double vwap = view.session_vwap(0);
            if (vwap <= 0.0) return sig;
            double dist = std::clamp(std::abs(c - vwap), st.p0 * sc, st.p1 * sc);
            if (c > vwap) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, dist, dist * st.p2, 0, 0, 0, 0, st.max_hold, 1, st.inst);
            }
            if (c < vwap) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, dist, dist * st.p2, 0, 0, 0, 0, st.max_hold, 1, st.inst);
            }
            return sig;
        }
        case F06_R2_FOMC_GLITCH: {
            if (!st.r_ready || em < 870 || em >= 930) return sig;
            if (h > st.r_hi) st.swept_hi = true;
            if (l < st.r_lo) st.swept_lo = true;
            if (st.swept_hi && c < st.r_hi) {
                double dist = std::clamp((h - c) + 5.0 * sc, st.p0 * sc, st.p1 * sc);
                st.swept_hi = false; st.trades_today++;
                return make_sig(-1, 0, 0.0, dist, dist * st.p2, 0, 0, 0, 0, st.max_hold, 1, st.inst);
            }
            if (st.swept_lo && c > st.r_lo) {
                double dist = std::clamp((c - l) + 5.0 * sc, st.p0 * sc, st.p1 * sc);
                st.swept_lo = false; st.trades_today++;
                return make_sig(1, 0, 0.0, dist, dist * st.p2, 0, 0, 0, 0, st.max_hold, 1, st.inst);
            }
            return sig;
        }
        case F06_R7_USMAN_ORB: {
            if (!view.or_formed(15) || !b5_just_closed) return sig;
            double oh = view.or_high(15), ol = view.or_low(15);
            double prev_c5 = st.c5_c[0];
            if (prev_c5 > oh) {
                double sl = std::clamp(prev_c5 - ol, 5.0 * sc, st.p2 * sc);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 1, st.inst);
            }
            if (prev_c5 < ol) {
                double sl = std::clamp(oh - prev_c5, 5.0 * sc, st.p2 * sc);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 1, st.inst);
            }
            return sig;
        }
        case F06_R10A_CIMI_30M:
        case F06_R10B_CIMI_15M:
        case F06_LIVE_R10A:
        case F06_LIVE_R10B: {
            bool is_30 = (st.kind == F06_R10A_CIMI_30M || st.kind == F06_LIVE_R10A);
            int or_m = is_30 ? 30 : 15;
            if (!view.or_formed(or_m)) return sig;
            double oh = view.or_high(or_m), ol = view.or_low(or_m);
            double buf = st.p2 * sc;
            if (st.kind == F06_LIVE_R10A || st.kind == F06_LIVE_R10B) {
                double sl = st.p3 * sc;
                if (c > oh + buf) {
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 1, st.inst);
                }
                if (!is_30 && c < ol - buf) {
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 1, st.inst);
                }
            } else {
                if (!b5_just_closed) return sig;
                double c5 = st.c5_c[0], l5 = st.c5_l[0];
                if (c5 > oh) {
                    double sl = std::max(5.0 * st.inst.tick_size, is_30 ? (c5 - l5) : (c5 - ol));
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 1, st.inst);
                }
                if (!is_30 && c5 < ol) {
                    double sl = std::max(5.0 * st.inst.tick_size, oh - c5);
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 1, st.inst);
                }
            }
            return sig;
        }
        case F06_R3_BRANDON_IFVG:
        case F06_R8_SEBASTIAN_50EQ:
        case F06_R9_ANTHONY_9PT:
        case F06_LIVE_R3:
        case F06_LIVE_R8:
        case F06_LIVE_R9: {
            int lb = std::clamp(static_cast<int>(std::round(st.p3)), 6, 40);
            double p_lo = 1e18, p_hi = -1e18;
            for (int k = 4; k <= lb + 3; ++k) {
                p_lo = std::min(p_lo, static_cast<double>(view[-k].l));
                p_hi = std::max(p_hi, static_cast<double>(view[-k].h));
            }
            double min_g = st.p0 * sc;
            // Bar -4 vs Bar -2 gap inverted by bar -1 and retested at bar 0
            double l4 = static_cast<double>(view[-4].l), h4 = static_cast<double>(view[-4].h);
            double l2 = static_cast<double>(view[-2].l), h2 = static_cast<double>(view[-2].h);
            double l3 = static_cast<double>(view[-3].l), h3 = static_cast<double>(view[-3].h);
            double l1 = static_cast<double>(view[-1].l), h1 = static_cast<double>(view[-1].h);
            double c1 = static_cast<double>(view[-1].c);

            bool eq_mode = (st.kind == F06_R8_SEBASTIAN_50EQ || st.kind == F06_LIVE_R8);
            double eq = 0.5 * (p_hi + p_lo);

            // Bullish IFVG (bearish FVG [h2, l4] inverted upward by bar -1 and retested bullish on bar 0)
            if (l3 < p_lo && (l4 - h2) >= min_g && c1 > l4 && l <= l4 && c > o) {
                double sl_px = std::min({l3, l2, l1});
                double sl = std::max(2.0 * sc, c - sl_px);
                double tp = eq_mode ? (eq - c) : (sl * st.p1);
                if (!eq_mode || tp > 2.0 * st.inst.tick_size) {
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, 45, 2, st.inst);
                }
            }
            // Bearish IFVG
            if (h3 > p_hi && (l2 - h4) >= min_g && c1 < h4 && h >= h4 && c < o) {
                double sl_px = std::max({h3, h2, h1});
                double sl = std::max(2.0 * sc, sl_px - c);
                double tp = eq_mode ? (c - eq) : (sl * st.p1);
                if (!eq_mode || tp > 2.0 * st.inst.tick_size) {
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, 45, 2, st.inst);
                }
            }
            return sig;
        }
        case F06_R5_8AM_CANDLE:
        case F06_LIVE_R5: {
            if (sess != 2) return sig;
            if (!st.r_ready) {
                if (scan_et_window_hl(view, 480, 540, 240, st.r_hi, st.r_lo)) {
                    st.r_ready = true;
                } else {
                    return sig;
                }
            }
            if (h > st.r_hi) { st.swept_hi = true; st.sweep_hi_px = std::max(st.sweep_hi_px, h); }
            if (l < st.r_lo) { st.swept_lo = true; st.sweep_lo_px = std::min(st.sweep_lo_px, l); }
            if (st.swept_hi && c < o && c < st.r_hi) {
                double sl = std::max(st.p0 * sc, st.sweep_hi_px - c);
                double tp = (st.kind == F06_LIVE_R5) ? (st.p1 * sc * 2.0) : std::max(2.0 * sc, c - st.r_lo);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 2, st.inst);
            }
            if (st.swept_lo && c > o && c > st.r_lo) {
                double sl = std::max(st.p0 * sc, c - st.sweep_lo_px);
                double tp = (st.kind == F06_LIVE_R5) ? (st.p1 * sc * 2.0) : std::max(2.0 * sc, st.r_hi - c);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 2, st.inst);
            }
            return sig;
        }
        case F06_R6_HUDSON_C2C3: {
            if (st.b5_count < 6 || !b5_just_closed) return sig;
            // c5_o[0], c5_h[0], c5_l[0], c5_c[0] is C3 (just closed); [1] is C2; [2] is C1
            double p_lo = 1e18, p_hi = -1e18;
            for (int k = 3; k < 12 && k < st.b5_count; ++k) {
                p_lo = std::min(p_lo, st.c5_l[k]);
                p_hi = std::max(p_hi, st.c5_h[k]);
            }
            if (p_lo >= p_hi) return sig;
            bool c1_sw_lo = st.c5_l[2] < p_lo;
            bool c2_bull = st.c5_c[1] > st.c5_o[1] && st.c5_c[1] > st.c5_c[2];
            bool c3_exp_up = st.c5_c[0] > st.c5_o[0] && st.c5_c[0] > st.c5_h[1];
            if (c1_sw_lo && c2_bull && c3_exp_up) {
                double cand = std::min(st.c5_l[1], st.c5_l[0]);
                double sl = std::clamp(st.c5_c[0] - cand, st.p1 * sc, st.p2 * sc);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p0, 0, 0, 0, 0, 90, 2, st.inst);
            }
            bool c1_sw_hi = st.c5_h[2] > p_hi;
            bool c2_bear = st.c5_c[1] < st.c5_o[1] && st.c5_c[1] < st.c5_c[2];
            bool c3_exp_dn = st.c5_c[0] < st.c5_o[0] && st.c5_c[0] < st.c5_l[1];
            if (c1_sw_hi && c2_bear && c3_exp_dn) {
                double cand = std::max(st.c5_h[1], st.c5_h[0]);
                double sl = std::clamp(cand - st.c5_c[0], st.p1 * sc, st.p2 * sc);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p0, 0, 0, 0, 0, 90, 2, st.inst);
            }
            return sig;
        }
        case F06_VIBE_VWAP: {
            double vwap = view.session_vwap(0);
            if (vwap <= 0.0) return sig;
            double th = st.p0 * sc, sl = st.p1 * sc, tp = st.p2 * sc;
            if (c - vwap >= th) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, 45, 2, st.inst);
            }
            if (vwap - c >= th) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, 45, 2, st.inst);
            }
            return sig;
        }
        case F06_LIVE_R4_MIDNIGHT: {
            if (!st.r_ready) return sig;
            double buf = st.p0 * sc, sl = st.p1 * sc, tp = st.p2 * sc;
            if (c > st.r_hi + buf) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 2, st.inst);
            }
            if (c < st.r_lo - buf) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 2, st.inst);
            }
            return sig;
        }
    }
    return sig;
}

template <F06Kind K>
inline void add_f06(
    std::vector<s546::RegisteredStrategy>& out,
    const std::string& uid,
    const std::string& name,
    const std::string& note,
    int32_t sess_mask,
    std::vector<u11::ParamSpec> params
) {
    s546::RegisteredStrategy r{};
    r.uid = uid;
    r.display_name = name;
    r.super_family = "06_Kernel_23_Reels_and_Creator_Adapters";
    r.source_note = note;
    r.fidelity = "EXACT_SOURCE";
    r.needs_cross_asset = false;
    r.session_mask = sess_mask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<F06State>(uid, f06_on_bar, f06_init<K>);
    out.push_back(std::move(r));
}

inline void register_f06(std::vector<s546::RegisteredStrategy>& out) {
    const std::string cat = "STRATEGY_CATALOG.md / kernel src";
    add_f06<F06_BB_FADE>(out, "bollinger_fade_test_adapter", "Bollinger Fade Test Scalp Adapter", "test_scalp_strategies.py", 0xF,
        {p_int("period", 20, 10, 40), p_lin("std_dev", 2.0, 1.5, 3.0), p_lin("sl_pts", 15.0, 8.0, 30.0), p_lin("tp_pts", 15.0, 8.0, 30.0), p_int("max_trades", 20, 5, 50)});
    add_f06<F06_CPP_UNIV>(out, "cpp_universal", "Universal C++ / Rust ABI Strategy Adapter", "cpp_universal_adapter.py / grand_champion_trio", 0xF,
        {p_int("or_mins", 30, 15, 60, 3), p_lin("hydraulic_th", 1.5, 0.8, 2.5), p_lin("tsallis_cut", 0.8, 0.4, 1.0), p_lin("volterra_cut", 3.0, 1.5, 5.0), p_lin("sl_pts", 36.25, 15.0, 50.0), p_lin("tp_pts", 60.0, 30.0, 90.0)});
    add_f06<F06_R11_FADE>(out, "kernel_reel_11_fade_shinobi", "Reel 11 Fade: Shinobi SD Fade (Opposite Continuation)", cat, 0xC,
        {p_int("swing_lb", 30, 15, 60), p_lin("fib_lo", 0.618, 0.50, 0.65), p_lin("fib_hi", 0.786, 0.70, 0.88), p_lin("sl_pts", 18.0, 10.0, 35.0), p_lin("rr", 2.0, 1.0, 4.0), p_int("max_trades", 4, 1, 8)});
    add_f06<F06_R11_SHINOBI>(out, "kernel_reel_11_shinobi_sd_ote", "Reel 11: Shinobi Signals NY SD + OTE Reversal", cat, 0xC,
        {p_int("swing_lb", 30, 15, 60), p_lin("fib_lo", 0.618, 0.50, 0.65), p_lin("fib_hi", 0.786, 0.70, 0.88), p_lin("sl_pts", 18.0, 10.0, 35.0), p_lin("rr", 2.0, 1.0, 4.0), p_int("max_trades", 4, 1, 8)});
    add_f06<F06_R12_TUNED>(out, "kernel_reel_12_tuned_zentrades", "Reel 12 Tuned: ZenTrades 15M Trampoline (4pt iFVG, 3.0 RR, Cap 2)", cat, 0xC,
        {p_lin("min_ifvg", 4.0, 2.0, 8.0), p_lin("sl_pts", 15.0, 8.0, 30.0), p_lin("rr", 3.0, 1.5, 4.5), p_int("swing_lb", 15, 8, 30), p_int("max_trades", 2, 1, 5)});
    add_f06<F06_R12_ZENTRADES>(out, "kernel_reel_12_zentrades_trampoline", "Reel 12: ZenTrades 15M Trampoline + LTF iFVG", cat, 0xC,
        {p_lin("min_ifvg", 1.5, 0.5, 5.0), p_lin("sl_pts", 15.0, 8.0, 30.0), p_lin("rr", 2.0, 1.0, 4.0), p_int("swing_lb", 15, 8, 30), p_int("max_trades", 5, 1, 8)});
    add_f06<F06_R13_EXT>(out, "kernel_reel_13_ext_vedachala", "Reel 13 Extended: Vedachala POI Extended Target (80pt TP / 25pt SL)", cat, 0xC,
        {p_int("poi_lb", 20, 10, 40), p_lin("sl_pts", 25.0, 15.0, 40.0), p_lin("tp_pts", 80.0, 40.0, 120.0), p_lin("fvg_min", 2.0, 0.5, 6.0), p_int("max_trades", 3, 1, 6)});
    add_f06<F06_R13_VEDACHALA>(out, "kernel_reel_13_vedachala_poi", "Reel 13: Vedachala MTF POI Scalper ($500 Target)", cat, 0xC,
        {p_int("poi_lb", 20, 10, 40), p_lin("sl_pts", 15.0, 8.0, 30.0), p_lin("tp_pts", 25.0, 15.0, 50.0), p_lin("fvg_min", 2.0, 0.5, 6.0), p_int("max_trades", 3, 1, 6)});
    add_f06<F06_R14_MDAY>(out, "kernel_reel_14_mday_quiet_conviction", "Reel 14 Multi-Day: Quiet Conviction Multi-Day Sweep (1R BE Trail)", cat, 0xF,
        {p_lin("sl_pts", 15.0, 8.0, 30.0), p_lin("rr", 2.5, 1.5, 4.0), p_lin("be_r", 1.0, 0.5, 1.5), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_R14_MONDAY>(out, "kernel_reel_14_quiet_conviction_monday", "Reel 14: Quiet Conviction Situational Monday Analysis", cat, 0xF,
        {p_lin("sl_pts", 15.0, 8.0, 30.0), p_lin("rr", 2.0, 1.0, 3.5), p_lin("be_r", 0.0, 0.0, 1.0), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_R15_MAFIA_3BAR>(out, "kernel_reel_15_breakout_mafia_orb", "Reel 15: Breakout Mafia ORB Control & 3-Bar Acceptance", cat, 0xC,
        {p_int("consec_bars", 3, 2, 5), p_lin("sl_pts", 20.0, 10.0, 35.0), p_lin("rr", 2.0, 1.0, 4.0), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_R16_FADE>(out, "kernel_reel_16_fade_rumers", "Reel 16 Fade: The Rumers Box Breakout (Opposite Continuation)", "run_all_46_reels_validation.py", 0xC,
        {p_lin("box_edge_frac", 0.15, 0.05, 0.30), p_lin("min_box_pts", 20.0, 10.0, 50.0), p_lin("sl_pts", 20.0, 10.0, 40.0), p_lin("rr", 2.0, 1.0, 3.5), p_int("max_trades", 3, 1, 6)});
    add_f06<F06_R16_RUMERS>(out, "kernel_reel_16_rumers_daily_box", "Reel 16: The Rumers Daily Box Theory Scalp", "run_all_46_reels_validation.py", 0xC,
        {p_lin("box_edge_frac", 0.15, 0.05, 0.30), p_lin("min_box_pts", 20.0, 10.0, 50.0), p_lin("sl_pts", 20.0, 10.0, 40.0), p_lin("rr", 2.0, 1.0, 3.5), p_int("max_trades", 3, 1, 6)});
    add_f06<F06_R1_AUSTIN_VWAP>(out, "kernel_reel_1_austin_vwap", "Reel 1: Austin McConnell 2nd Candle VWAP Breakout", "austin_vwap.py", 0x4,
        {p_lin("min_sl", 10.0, 5.0, 20.0), p_lin("max_sl", 30.0, 20.0, 60.0), p_lin("rr", 2.0, 1.0, 3.5), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_R2_FOMC_GLITCH>(out, "kernel_reel_2_fomc_glitch", "Reel 2: FOMC Glitch Sweep & Fade", "fomc_glitch.py", 0x8,
        {p_lin("min_sl", 15.0, 8.0, 25.0), p_lin("max_sl", 40.0, 25.0, 60.0), p_lin("rr", 2.0, 1.5, 3.5), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_R7_USMAN_ORB>(out, "kernel_reel_7_usman_ashraf_orb", "Reel 7: Usman Ashraf 15M Classic ORB", "usman_ashraf_orb.py", 0xC,
        {p_int("or_mins", 15, 15, 30, 2), p_lin("rr", 2.0, 1.0, 3.5), p_lin("max_sl_pts", 40.0, 20.0, 120.0), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_R10A_CIMI_30M>(out, "reel_10a_cimi_30m_long", "Reel 10A: Andrea Cimi 30m Long-Only ORB", "cimi_orb.py", 0xC,
        {p_int("or_mins", 30, 15, 30, 2), p_lin("rr", 1.0, 0.5, 2.5), p_lin("buf", 0.0, 0.0, 10.0), p_lin("sl_pts", 25.0, 10.0, 40.0), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_R10B_CIMI_15M>(out, "reel_10b_cimi_15m_both", "Reel 10B: Andrea Cimi 15m Both Directions (0.5R)", "cimi_orb.py", 0xC,
        {p_int("or_mins", 15, 15, 30, 2), p_lin("rr", 0.5, 0.3, 1.5), p_lin("buf", 0.0, 0.0, 10.0), p_lin("sl_pts", 20.0, 10.0, 40.0), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_R15_MAFIA_1M>(out, "reel_15_1m_breakout_mafia", "Reel 15 1M Tuned: Breakout Mafia 1M ORB Acceptance", cat, 0xC,
        {p_int("consec_bars", 1, 1, 3), p_lin("sl_pts", 20.0, 10.0, 35.0), p_lin("rr", 2.0, 1.0, 4.0), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_R3_BRANDON_IFVG>(out, "reel_3_mtf_1m_ifvg", "Reel 3: Brandon K MTF 1m IFVG Retest", "run_real_10reels_validation.py", 0xC,
        {p_lin("min_fvg", 5.0, 2.0, 10.0), p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_pts", 15.0, 8.0, 30.0), p_int("swing_lb", 10, 6, 20), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_R5_8AM_CANDLE>(out, "reel_5_8am_candle", "Reel 5: Sky Trades 8:00 AM Candle Strategy", "run_real_10reels_validation.py", 0x4,
        {p_lin("min_risk", 2.0, 1.0, 10.0), p_lin("sl_pts", 18.0, 10.0, 35.0), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_R6_HUDSON_C2C3>(out, "reel_6_hudson_c2c3", "Reel 6: Hudson Trades C2/C3 Continuation Model", "hudson_c2c3.py", 0xC,
        {p_lin("rr", 5.0, 2.0, 7.0), p_lin("min_sl", 8.0, 4.0, 15.0), p_lin("max_sl", 25.0, 15.0, 45.0), p_int("max_trades", 3, 1, 5)});
    add_f06<F06_R8_SEBASTIAN_50EQ>(out, "reel_8_50pct_eq_ifvg", "Reel 8: Sebastian 50% Equilibrium IFVG", "run_real_10reels_validation.py", 0xC,
        {p_lin("min_fvg", 5.0, 2.0, 10.0), p_lin("rr", 1.5, 0.8, 2.5), p_lin("sl_pts", 14.0, 8.0, 25.0), p_int("swing_lb", 10, 6, 20), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_R9_ANTHONY_9PT>(out, "reel_9_9pt_disp_ifvg", "Reel 9: Anthony Oksen >=9pt Displacement IFVG", "run_real_10reels_validation.py", 0xC,
        {p_lin("min_fvg", 9.0, 5.0, 14.0), p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_pts", 15.0, 8.0, 30.0), p_int("swing_lb", 10, 6, 20), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_VIBE_VWAP>(out, "vibe_vwap_scalp", "Example Vibe VWAP Scalp (Template)", "example_vibe_strategy.py", 0xF,
        {p_lin("threshold_pts", 15.0, 8.0, 30.0), p_lin("sl_pts", 10.0, 5.0, 25.0), p_lin("tp_pts", 15.0, 8.0, 35.0), p_int("max_trades", 6, 2, 15)});
    add_f06<F06_LIVE_R10A>(out, "kernel_reel_10a", "Reel 10A: Andrea Cimi 30m Long-Only ORB", "bt_orb/v5/strategy_registry.py", 0xC,
        {p_int("or_mins", 30, 15, 30, 2), p_lin("rr", 1.0, 0.5, 2.5), p_lin("buffer_pts", 10.0, 2.0, 20.0), p_lin("sl_pts", 25.0, 12.0, 40.0), p_int("max_trades", 1, 1, 2)});
    add_f06<F06_LIVE_R10B>(out, "kernel_reel_10b", "Reel 10B: Andrea Cimi 15m 0.5 RR ORB", "bt_orb/v5/strategy_registry.py", 0xC,
        {p_int("or_mins", 15, 15, 30, 2), p_lin("rr", 0.5, 0.3, 1.5), p_lin("buffer_pts", 8.0, 2.0, 16.0), p_lin("sl_pts", 20.0, 10.0, 35.0), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_LIVE_R3>(out, "kernel_reel_3", "Reel 3: Brandon K Multi-TF 1m IFVG", "bt_orb/v5/strategy_registry.py", 0xC,
        {p_lin("min_fvg", 4.0, 2.0, 8.0), p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_pts", 15.0, 8.0, 30.0), p_int("swing_lb", 12, 6, 24), p_int("max_trades", 3, 1, 5)});
    add_f06<F06_LIVE_R4_MIDNIGHT>(out, "kernel_reel_4", "Reel 4: John Wickwatcher Midnight 1H", "bt_orb/v5/strategy_registry.py", 0x3,
        {p_lin("buffer_pts", 8.0, 2.0, 16.0), p_lin("sl_pts", 20.0, 10.0, 40.0), p_lin("tp_pts", 25.0, 15.0, 50.0), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_LIVE_R5>(out, "kernel_reel_5", "Reel 5: Sky Trades 8:00 AM One Candle", "bt_orb/v5/strategy_registry.py", 0x4,
        {p_lin("buffer_pts", 6.0, 2.0, 14.0), p_lin("sl_pts", 18.0, 10.0, 35.0), p_int("max_trades", 2, 1, 4)});
    add_f06<F06_LIVE_R8>(out, "kernel_reel_8", "Reel 8: Sebastian 50% Equilibrium IFVG", "bt_orb/v5/strategy_registry.py", 0xC,
        {p_lin("min_fvg", 4.0, 2.0, 8.0), p_lin("rr", 1.5, 0.8, 2.5), p_lin("sl_pts", 14.0, 8.0, 25.0), p_int("swing_lb", 12, 6, 24), p_int("max_trades", 3, 1, 5)});
    add_f06<F06_LIVE_R9>(out, "kernel_reel_9", "Reel 9: Anthony Oksen >=9pt Displacement IFVG", "bt_orb/v5/strategy_registry.py", 0xC,
        {p_lin("min_fvg", 9.0, 5.0, 14.0), p_lin("rr", 2.0, 1.0, 3.5), p_lin("sl_pts", 15.0, 8.0, 30.0), p_int("swing_lb", 12, 6, 24), p_int("max_trades", 3, 1, 5)});
}


// ============================================================================
// FAMILY 08: 08_AGY_Kimi_Chat_Archived_Historical_CPP_Strategies (64 UIDs)
// Sources: chat_arch_casper_ifvg_5m + line2788 / line2792 / line2798 / line2886
//          + line5914 / line5932 / line5948 / test_20_novel / test_batch11
// ============================================================================

enum F08Kind {
    // 01..24: Casper + Fisher / Kalman / ZScore-MAD / Tokyo-NY / Settlement
    F08_CASPER_IFVG_5M = 0,
    F08_FISHER_BASE_ALL_1C,
    F08_FISHER_INFO_DYN_NQ,
    F08_FISHER_TWOSTAGE_NQ,
    F08_FISHER_INFO_VOLT_NQ,
    F08_FISHER_SKIPLDN_1C,
    F08_KALMAN_REGIME_NQ,
    F08_TOKYO_NY_FLOW_NQ,
    F08_KALMAN_LATENT_VEL_NQ,
    F08_ZSCORE_MAD_DYN_NQ,
    F08_FISHER_NY_HIGHCONV_1C,
    F08_ZSCORE_MAD_INST_ES,
    F08_FISHER_KALMAN_ASNY_1C,
    F08_KALMAN_HIGHBETA_YM,
    F08_FISHER_SKIPLDN_VOLT_2C,
    F08_SETTLEMENT_TOKYO_REV_NQ,
    F08_FISHER_TWOSTAGE_ES,
    F08_FISHER_KALMAN_VOLT_2C,
    F08_ZSCORE_MAD_DYN_ES,
    F08_KALMAN_REGIME_ES,
    F08_FISHER_TWOSTAGE_YM,
    F08_KALMAN_REGIME_YM,
    F08_ZSCORE_MAD_DYN_YM,
    F08_FISHER_INFO_METRIC_NQ_ALL,
    // 25..64: Batch 10 & Batch 11 Novel Math / Microstructure (38+2 = 40)
    F08_201_DELTA_ABSORP_DIV,
    F08_201_DELTA_DIV_TRAP,
    F08_201_SWEEP_WICK_PIVOT,
    F08_202_OVERNIGHT_FAIL_AUC,
    F08_202_POISSON_ARRIVAL,
    F08_203_WIENER_ENTROPY,
    F08_203_TRIPLE_INSIDE_COIL,
    F08_204_CA_COINT_SPREAD,
    F08_204_CA_LEADLAG_ACCEL,
    F08_204_CA_VEL_DIV,
    F08_205_OB_ABSORP_SHELF,
    F08_206_FDI_PERSISTENCE,
    F08_206_VWAP_3SIG_EXH,
    F08_207_OPEN_DRIVE_TRAP,
    F08_208_LOWVOL_VACUUM_GLIDE,
    F08_209_BB_PINCH_EXPLODE,
    F08_210_BLOCK_MOM_IGNIT,
    F08_211_WYCKOFF_SPRING,
    F08_212_KL_INFO_SHOCK,
    F08_212_MIDDAY_LUNCH_COIL,
    F08_213_DOUBLE_EXH_WICK,
    F08_214_CVD_ICEBERG_TRAP,
    F08_214_KURTOSIS_FATTAIL,
    F08_214_MOC_1530_CASCADE,
    F08_215_FVG_VACUUM_RETEST,
    F08_216_DYN_ATR_SNAPBACK,
    F08_216_HOLDER_SINGULARITY,
    F08_216_JUMPDIFF_SHOCK,
    F08_217_NR7_TREND_EXP,
    F08_218_MICRO_TREND_BRK,
    F08_219_VWAP_REC_VEL,
    F08_219_VWAP_REC_VOL,
    F08_220_RUNLEN_COMPLEXITY,
    F08_222_MINKOWSKI_CONE,
    F08_224_ERGODIC_FLOW,
    F08_225_HAWKES_EXCIT,
    F08_229_QUEUING_OVERFLOW,
    F08_230_PERCOLATION_HWY,
    F08_233_VANDERPOL_RUPTURE,
    F08_235_PARETO_LEVY
};

template <typename T, size_t Cap>
struct RingBuf {
    T data[Cap]{};
    int cnt = 0;
    int head = 0;
    void push(const T& v) {
        data[head] = v;
        head = (head + 1) % static_cast<int>(Cap);
        if (cnt < static_cast<int>(Cap)) cnt++;
    }
    int size() const { return cnt; }
    const T& back(int k = 0) const {
        int idx = ((head - 1 - k) % static_cast<int>(Cap) + static_cast<int>(Cap)) % static_cast<int>(Cap);
        return data[idx];
    }
};

struct F08State {
    F08Kind kind = F08_FISHER_BASE_ALL_1C;
    u11::InstrumentSpec inst{};
    double p0 = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0;
    int max_hold = 45;
    int max_trades_day = 8;

    int last_day = -1;
    int trades_today = 0;
    int wins_today = 0;

    // Track position transitions causally for stop-on-first-win (Casper)
    bool prev_in_pos = false;
    int8_t active_dir = 0;
    double active_entry = 0.0;

    // Causal Kalman state (line2788 / line2792 / line2798)
    bool kal_init = false;
    double kal_x = 0.0;
    double kal_p = 1.0;

    // Causal 5m aggregator & IFVG list for Casper 5m
    int cur_5m_bucket = -1;
    double b5_o = 0.0, b5_h = 0.0, b5_l = 0.0, b5_c = 0.0;
    struct Bar5m { double o, h, l, c; };
    RingBuf<Bar5m, 8> bars5m;
    struct FvgZone { double top, bot; int8_t dir; bool valid; };
    FvgZone fvg_zones[4]{};
    int fvg_head = 0;

    // Overnight & opening drive trackers (202, 207)
    double on_h = -1e18, on_l = 1e18;
    bool on_valid = false;
    int sess_bars = 0;
    int last_sess = -99;
    double op_h = -1e18, op_l = 1e18;
    int op_bars = 0;
};

template <F08Kind K>
inline void f08_init(F08State& st, const std::vector<double>& params, const u11::InstrumentSpec* spec) {
    st = F08State{};
    st.kind = K;
    if (spec) st.inst = *spec;
    switch (K) {
        case F08_CASPER_IFVG_5M:
            st.p0 = p_at(params, 0, 2.0);   // FVG threshold (NQ pts)
            st.p1 = p_at(params, 1, 22.5);  // SL pts
            st.p2 = p_at(params, 2, 35.0);  // TP pts
            st.p3 = p_at(params, 3, 1.0);   // Stop on 1st daily win (>0.5)
            st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 3.0)));
            st.max_hold = 60;
            break;
        case F08_FISHER_BASE_ALL_1C:
        case F08_FISHER_SKIPLDN_1C:
        case F08_FISHER_NY_HIGHCONV_1C:
        case F08_FISHER_KALMAN_ASNY_1C:
        case F08_FISHER_SKIPLDN_VOLT_2C:
        case F08_FISHER_KALMAN_VOLT_2C:
        case F08_FISHER_INFO_DYN_NQ:
        case F08_FISHER_INFO_VOLT_NQ:
        case F08_FISHER_TWOSTAGE_NQ:
        case F08_FISHER_TWOSTAGE_ES:
        case F08_FISHER_TWOSTAGE_YM:
        case F08_FISHER_INFO_METRIC_NQ_ALL: {
            double def_ft = (K == F08_FISHER_NY_HIGHCONV_1C) ? 4.0e5
                          : (K == F08_FISHER_KALMAN_ASNY_1C || K == F08_FISHER_KALMAN_VOLT_2C ||
                             K == F08_FISHER_TWOSTAGE_NQ || K == F08_FISHER_TWOSTAGE_YM) ? 4.5e5
                          : 5.0e5;
            double def_mr = (K == F08_FISHER_NY_HIGHCONV_1C) ? 1.2e-4 : 1.0e-4;
            double def_sl = (K == F08_FISHER_NY_HIGHCONV_1C || K == F08_FISHER_KALMAN_ASNY_1C ||
                             K == F08_FISHER_KALMAN_VOLT_2C || K == F08_FISHER_TWOSTAGE_NQ ||
                             K == F08_FISHER_TWOSTAGE_ES || K == F08_FISHER_TWOSTAGE_YM) ? 1.1
                          : (K == F08_FISHER_INFO_METRIC_NQ_ALL) ? 1.05 : 1.2;
            double def_tp = (K == F08_FISHER_NY_HIGHCONV_1C || K == F08_FISHER_KALMAN_ASNY_1C ||
                             K == F08_FISHER_KALMAN_VOLT_2C || K == F08_FISHER_TWOSTAGE_ES ||
                             K == F08_FISHER_TWOSTAGE_YM) ? 2.8
                          : (K == F08_FISHER_TWOSTAGE_NQ) ? 3.2
                          : (K == F08_FISHER_INFO_METRIC_NQ_ALL) ? 2.85 : 2.5;
            st.p0 = p_at(params, 0, def_ft);
            st.p1 = p_at(params, 1, def_mr);
            st.p2 = p_at(params, 2, def_sl);
            st.p3 = p_at(params, 3, def_tp);
            st.p4 = p_at(params, 4, (K == F08_FISHER_TWOSTAGE_NQ) ? 1.8 : 1.5); // s2 trig ATR
            st.p5 = p_at(params, 5, (K == F08_FISHER_TWOSTAGE_NQ) ? 0.6 : 0.5); // s2 trail/lock
            st.max_hold = static_cast<int>(std::round(p_at(params, 6, 45.0)));
            break;
        }
        case F08_KALMAN_REGIME_NQ:
        case F08_KALMAN_LATENT_VEL_NQ:
        case F08_KALMAN_HIGHBETA_YM:
        case F08_KALMAN_REGIME_ES:
        case F08_KALMAN_REGIME_YM: {
            double def_tp = (K == F08_KALMAN_REGIME_NQ) ? 2.8
                          : (K == F08_KALMAN_REGIME_ES || K == F08_KALMAN_REGIME_YM) ? 2.6 : 2.5;
            double def_sl = (K == F08_KALMAN_REGIME_ES || K == F08_KALMAN_REGIME_YM) ? 1.1 : 1.0;
            st.p0 = p_at(params, 0, 0.015);  // Q_kal
            st.p1 = p_at(params, 1, 2.0);    // residual ATR mult
            st.p2 = p_at(params, 2, def_sl);
            st.p3 = p_at(params, 3, def_tp);
            st.p4 = p_at(params, 4, 0.95);   // ATR expansion ratio (NQ gated)
            st.max_hold = static_cast<int>(std::round(p_at(params, 5, 45.0)));
            break;
        }
        case F08_ZSCORE_MAD_DYN_NQ:
        case F08_ZSCORE_MAD_INST_ES:
        case F08_ZSCORE_MAD_DYN_ES:
        case F08_ZSCORE_MAD_DYN_YM: {
            double def_z = (K == F08_ZSCORE_MAD_INST_ES) ? 2.2 : (K == F08_ZSCORE_MAD_DYN_NQ ? 2.4 : 2.5);
            double def_sl = (K == F08_ZSCORE_MAD_INST_ES) ? 1.0 : 1.2;
            double def_tp = (K == F08_ZSCORE_MAD_INST_ES) ? 1.8 : 2.2;
            st.p0 = p_at(params, 0, 30.0);
            st.p1 = p_at(params, 1, def_z);
            st.p2 = p_at(params, 2, 0.4);
            st.p3 = p_at(params, 3, def_sl);
            st.p4 = p_at(params, 4, def_tp);
            st.max_hold = 45;
            break;
        }
        case F08_TOKYO_NY_FLOW_NQ:
            st.p0 = p_at(params, 0, 20.0);
            st.p1 = p_at(params, 1, 1.5);
            st.p2 = p_at(params, 2, 1.1);
            st.p3 = p_at(params, 3, 3.0);
            st.max_hold = 45;
            break;
        case F08_SETTLEMENT_TOKYO_REV_NQ:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 2.2);
            st.p2 = p_at(params, 2, 1.2);
            st.p3 = p_at(params, 3, 2.6);
            st.max_hold = 45;
            break;
        default: {
            // Batch 10 (target_r ~ 7.5), Batch 11 (target_r ~ 20.0), or CVD Iceberg (target_r ~ 2.2)
            double def_rr = (K >= F08_222_MINKOWSKI_CONE) ? 20.0
                          : (K == F08_214_CVD_ICEBERG_TRAP) ? 2.2
                          : 7.5;
            st.p0 = p_at(params, 0, def_rr);
            st.p1 = p_at(params, 1, 1.0);  // sensitivity / threshold scale
            st.p2 = p_at(params, 2, 1.0);  // sl clamp scale
            st.p3 = p_at(params, 3, 0.0);  // optional trailing R trigger (0 = pure RR / schema sweepable)
            st.p4 = p_at(params, 4, 0.0);  // optional trailing distance
            st.max_hold = static_cast<int>(std::round(p_at(params, 5, 90.0)));
            st.max_trades_day = 6;
            break;
        }
    }
}

inline double f08_fisher_20(const u11::CausalBarView& view, int lb, double& mean_r_out) {
    double sum_r = 0.0, sum_sq = 0.0;
    for (int k = 1; k <= lb; ++k) {
        double c_prev = static_cast<double>(view[-k - 1].c);
        double c_cur = static_cast<double>(view[-k].c);
        if (c_prev <= 0.0) return 0.0;
        double r = (c_cur - c_prev) / c_prev;
        sum_r += r;
        sum_sq += r * r;
    }
    mean_r_out = sum_r / static_cast<double>(lb);
    double var_r = (sum_sq / static_cast<double>(lb)) - (mean_r_out * mean_r_out);
    if (var_r <= 1e-9) return 0.0;
    return 1.0 / var_r;
}

inline double f08_kalman_update(F08State& st, double prev_c, double cur_atr, double q_kal) {
    if (!st.kal_init) {
        st.kal_x = prev_c;
        st.kal_p = 1.0;
        st.kal_init = true;
    }
    double r_kal = std::max(1e-4, cur_atr * cur_atr);
    st.kal_p += q_kal;
    double k_gain = st.kal_p / (st.kal_p + r_kal);
    st.kal_x = st.kal_x + k_gain * (prev_c - st.kal_x);
    st.kal_p = (1.0 - k_gain) * st.kal_p;
    return prev_c - st.kal_x;
}

inline u11::PluginSignal f08_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto& st = *static_cast<F08State*>(state_ptr);
    u11::PluginSignal sig{};

    if (view.day_yyyymmdd() != st.last_day) {
        st.last_day = view.day_yyyymmdd();
        st.trades_today = 0;
        st.wins_today = 0;
        st.on_h = -1e18;
        st.on_l = 1e18;
        st.on_valid = false;
        st.fvg_head = 0;
        for (int z = 0; z < 4; ++z) st.fvg_zones[z].valid = false;
    }

    int sess = view.session_id();
    if (sess != st.last_sess) {
        st.last_sess = sess;
        st.sess_bars = 0;
        st.op_h = -1e18;
        st.op_l = 1e18;
        st.op_bars = 0;
    }
    st.sess_bars++;

    const auto& b = view[0];
    double o = static_cast<double>(b.o), h = static_cast<double>(b.h);
    double l = static_cast<double>(b.l), c = static_cast<double>(b.c);
    double v = static_cast<double>(b.v);

    // Track overnight Asian + London high/low before NY open
    if (sess == 0 || sess == 1) {
        st.on_h = std::max(st.on_h, h);
        st.on_l = std::min(st.on_l, l);
        st.on_valid = true;
    }
    if (st.op_bars < 5) {
        st.op_h = std::max(st.op_h, h);
        st.op_l = std::min(st.op_l, l);
        st.op_bars++;
    }

    // Track win transition for Casper
    if (st.prev_in_pos && !view.in_position && st.active_dir != 0) {
        double pnl_pts = (st.active_dir == 1) ? (c - st.active_entry) : (st.active_entry - c);
        if (pnl_pts > 0.0) st.wins_today++;
        st.active_dir = 0;
    }
    st.prev_in_pos = view.in_position;

    // Update 5m candle buffer causally for Casper IFVG
    int em = view.et_mins();
    int bucket5 = view.day_yyyymmdd() * 300 + (em / 5);
    if (bucket5 != st.cur_5m_bucket) {
        st.cur_5m_bucket = bucket5;
        st.b5_o = o; st.b5_h = h; st.b5_l = l; st.b5_c = c;
    } else {
        st.b5_h = std::max(st.b5_h, h);
        st.b5_l = std::min(st.b5_l, l);
        st.b5_c = c;
    }

    if (view.bar_index() < 65) return sig;
    double cur_atr = std::max(view.atr14(0), 2.0 * st.inst.tick_size);
    double pt_s = sym_pt_scale(st.inst);
    double sc = rel_scale(view, st.inst);

    // Keep Kalman filter continuously updated on bar -1 close before returning if in_position
    bool needs_kal = (st.kind == F08_FISHER_KALMAN_ASNY_1C || st.kind == F08_FISHER_KALMAN_VOLT_2C ||
                      st.kind == F08_KALMAN_REGIME_NQ || st.kind == F08_KALMAN_LATENT_VEL_NQ ||
                      st.kind == F08_KALMAN_HIGHBETA_YM || st.kind == F08_KALMAN_REGIME_ES ||
                      st.kind == F08_KALMAN_REGIME_YM);
    double kal_res = 0.0;
    if (needs_kal) {
        double qk = (st.kind >= F08_KALMAN_REGIME_NQ && st.kind <= F08_KALMAN_REGIME_YM) ? st.p0 : 0.015;
        kal_res = f08_kalman_update(st, static_cast<double>(view[-1].c), cur_atr, qk);
    }

    // Complete 5m bar & FVG scan for Casper
    bool completed_5m = false;
    if (st.kind == F08_CASPER_IFVG_5M && (em % 5 == 4)) {
        st.bars5m.push({st.b5_o, st.b5_h, st.b5_l, st.b5_c});
        completed_5m = true;
        if (st.bars5m.size() >= 3) {
            const auto& m0 = st.bars5m.back(0);
            const auto& m2 = st.bars5m.back(2);
            double min_g = st.p0 * pt_s;
            if (m0.l > m2.h + min_g) {
                st.fvg_zones[st.fvg_head] = F08State::FvgZone{m0.l, m2.h, static_cast<int8_t>(1), true};
                st.fvg_head = (st.fvg_head + 1) & 3;
            } else if (m2.l > m0.h + min_g) {
                st.fvg_zones[st.fvg_head] = F08State::FvgZone{m2.l, m0.h, static_cast<int8_t>(-1), true};
                st.fvg_head = (st.fvg_head + 1) & 3;
            }
        }
    }

    if (view.in_position || st.trades_today >= st.max_trades_day) return sig;

    // ------------------------------------------------------------------
    // 1) Casper 5m IFVG
    // ------------------------------------------------------------------
    if (st.kind == F08_CASPER_IFVG_5M) {
        if (!completed_5m) return sig;
        if (st.p3 > 0.5 && st.wins_today >= 1) return sig;
        if (sess != 2 && sess != 3) return sig;
        double sl = st.p1 * pt_s, tp = st.p2 * pt_s;
        for (int z = 0; z < 4; ++z) {
            auto& fz = st.fvg_zones[z];
            if (!fz.valid) continue;
            if (fz.dir == -1 && st.b5_c > fz.top) {
                fz.valid = false;
                st.trades_today++;
                st.active_dir = 1; st.active_entry = c;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            if (fz.dir == 1 && st.b5_c < fz.bot) {
                fz.valid = false;
                st.trades_today++;
                st.active_dir = -1; st.active_entry = c;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
        }
        return sig;
    }

    // ------------------------------------------------------------------
    // 2) Fisher Family (lines 2788, 2792, 2798, 2886)
    // ------------------------------------------------------------------
    if (st.kind == F08_FISHER_BASE_ALL_1C || st.kind == F08_FISHER_INFO_DYN_NQ ||
        st.kind == F08_FISHER_TWOSTAGE_NQ || st.kind == F08_FISHER_INFO_VOLT_NQ ||
        st.kind == F08_FISHER_SKIPLDN_1C || st.kind == F08_FISHER_NY_HIGHCONV_1C ||
        st.kind == F08_FISHER_KALMAN_ASNY_1C || st.kind == F08_FISHER_SKIPLDN_VOLT_2C ||
        st.kind == F08_FISHER_TWOSTAGE_ES || st.kind == F08_FISHER_KALMAN_VOLT_2C ||
        st.kind == F08_FISHER_TWOSTAGE_YM || st.kind == F08_FISHER_INFO_METRIC_NQ_ALL) {
        // Session gating per strategy
        if (st.kind == F08_FISHER_NY_HIGHCONV_1C) {
            if (!(sess == 2 && em >= 580 && em <= 940) && sess != 3) return sig;
        } else if (st.kind != F08_FISHER_BASE_ALL_1C && st.kind != F08_FISHER_INFO_METRIC_NQ_ALL) {
            // Skip London: Asian (0) or NY (2,3)
            if (sess == 1) return sig;
        }
        double mean_r = 0.0;
        double fi = f08_fisher_20(view, 20, mean_r);
        if (fi <= st.p0 || std::abs(mean_r) <= st.p1) return sig;
        int8_t dir = (mean_r > 0.0) ? 1 : -1;
        if (st.kind == F08_FISHER_KALMAN_ASNY_1C || st.kind == F08_FISHER_KALMAN_VOLT_2C) {
            if (dir == 1 && kal_res <= 0.0) return sig;
            if (dir == -1 && kal_res >= 0.0) return sig;
        }
        double sl = st.p2 * cur_atr;
        double tp = st.p3 * cur_atr;
        double r_trig = 0.0, r_trail = 0.0, be_trig = 0.0, be_off = 0.0;
        if (st.kind == F08_FISHER_TWOSTAGE_NQ || st.kind == F08_FISHER_TWOSTAGE_ES || st.kind == F08_FISHER_TWOSTAGE_YM) {
            be_trig = st.p4 * cur_atr;
            be_off = st.p5 * cur_atr;
        }
        st.trades_today++;
        return make_sig(dir, 0, 0.0, sl, tp, r_trig, r_trail, be_trig, be_off, st.max_hold, 2, st.inst);
    }

    // ------------------------------------------------------------------
    // 3) Kalman Family (line 2788, 2798)
    // ------------------------------------------------------------------
    if (st.kind == F08_KALMAN_REGIME_NQ || st.kind == F08_KALMAN_LATENT_VEL_NQ ||
        st.kind == F08_KALMAN_HIGHBETA_YM || st.kind == F08_KALMAN_REGIME_ES ||
        st.kind == F08_KALMAN_REGIME_YM) {
        if (st.kind == F08_KALMAN_REGIME_NQ) {
            double past_atr = std::max(view.atr14(-30), 2.0 * st.inst.tick_size);
            if (cur_atr <= st.p4 * past_atr) return sig;
        }
        if (std::abs(kal_res) <= st.p1 * cur_atr) return sig;
        int8_t dir = (kal_res > 0.0) ? 1 : -1;
        double sl = st.p2 * cur_atr;
        double tp = st.p3 * cur_atr;
        double be_trig = (st.kind == F08_KALMAN_REGIME_NQ || st.kind == F08_KALMAN_REGIME_ES || st.kind == F08_KALMAN_REGIME_YM)
                       ? 1.4 * cur_atr : 0.0;
        double be_off = (be_trig > 0.0) ? 0.4 * cur_atr : 0.0;
        st.trades_today++;
        return make_sig(dir, 0, 0.0, sl, tp, 0, 0, be_trig, be_off, st.max_hold, 2, st.inst);
    }

    // ------------------------------------------------------------------
    // 4) ZScore MAD Family (line 2788, 2798)
    // ------------------------------------------------------------------
    if (st.kind == F08_ZSCORE_MAD_DYN_NQ || st.kind == F08_ZSCORE_MAD_INST_ES ||
        st.kind == F08_ZSCORE_MAD_DYN_ES || st.kind == F08_ZSCORE_MAD_DYN_YM) {
        if (st.kind == F08_ZSCORE_MAD_DYN_NQ && sess == 1) return sig; // Asian + NY
        int win = std::clamp(static_cast<int>(std::round(st.p0)), 15, 60);
        int mid = win / 2;
        double med = static_cast<double>(view[-mid].c);
        double mad = 0.0;
        for (int k = 1; k <= win; ++k) mad += std::abs(static_cast<double>(view[-k].c) - med);
        mad /= static_cast<double>(win);
        if (mad <= st.p2 * cur_atr) return sig;
        double z_mad = (static_cast<double>(view[-1].c) - med) / mad;
        int8_t dir = 0;
        if (z_mad > st.p1) dir = -1;
        else if (z_mad < -st.p1) dir = 1;
        if (dir == 0) return sig;
        double sl = st.p3 * cur_atr;
        double tp = st.p4 * cur_atr;
        double be_trig = (st.kind != F08_ZSCORE_MAD_INST_ES) ? 1.2 * cur_atr : 0.0;
        double be_off = (st.kind != F08_ZSCORE_MAD_INST_ES) ? 0.3 * cur_atr : 0.0;
        st.trades_today++;
        return make_sig(dir, 0, 0.0, sl, tp, 0, 0, be_trig, be_off, st.max_hold, 2, st.inst);
    }

    // ------------------------------------------------------------------
    // 5) Tokyo NY Flow & Settlement Tokyo Reversion
    // ------------------------------------------------------------------
    if (st.kind == F08_TOKYO_NY_FLOW_NQ) {
        bool valid_t = (em >= 1200 && em <= 1350) || (em >= 0 && em <= 150) || (em >= 570 && em <= 690);
        if (!valid_t) return sig;
        int lb = std::clamp(static_cast<int>(std::round(st.p0)), 10, 40);
        double ret = static_cast<double>(view[-1].c) - static_cast<double>(view[-lb].c);
        double hi = -1e18, lo = 1e18;
        for (int k = 1; k <= lb; ++k) {
            hi = std::max(hi, static_cast<double>(view[-k].h));
            lo = std::min(lo, static_cast<double>(view[-k].l));
        }
        int8_t dir = 0;
        double c1 = static_cast<double>(view[-1].c);
        if (c1 > hi - 0.1 * cur_atr && ret > st.p1 * cur_atr) dir = 1;
        else if (c1 < lo + 0.1 * cur_atr && ret < -st.p1 * cur_atr) dir = -1;
        if (dir == 0) return sig;
        st.trades_today++;
        return make_sig(dir, 0, 0.0, st.p2 * cur_atr, st.p3 * cur_atr, 0, 0, 0, 0, st.max_hold, 2, st.inst);
    }
    if (st.kind == F08_SETTLEMENT_TOKYO_REV_NQ) {
        bool valid_t = (em >= 1200 && em <= 1380) || (em >= 0 && em <= 120);
        if (!valid_t) return sig;
        int lb = std::clamp(static_cast<int>(std::round(st.p0)), 8, 30);
        double ret = static_cast<double>(view[-1].c) - static_cast<double>(view[-lb].c);
        int8_t dir = 0;
        if (ret < -st.p1 * cur_atr) dir = 1;
        else if (ret > st.p1 * cur_atr) dir = -1;
        if (dir == 0) return sig;
        st.trades_today++;
        return make_sig(dir, 0, 0.0, st.p2 * cur_atr, st.p3 * cur_atr, 0, 0, 1.4 * cur_atr, 0.4 * cur_atr, st.max_hold, 2, st.inst);
    }

    // ------------------------------------------------------------------
    // 6) Batch 10 & Batch 11 Quant Microstructure & Physics Models (201..235)
    // ------------------------------------------------------------------
    bool bull = false, bear = false;
    double proposed_r = 6.0 * sc;
    double target_r = st.p0;
    double sens = st.p1;
    double vwap = view.session_vwap(0);
    double c1 = static_cast<double>(view[-1].c), c2 = static_cast<double>(view[-2].c);
    double c3 = static_cast<double>(view[-3].c), c4 = static_cast<double>(view[-4].c);
    double c5 = static_cast<double>(view[-5].c);
    double o1 = static_cast<double>(view[-1].o), h1 = static_cast<double>(view[-1].h), l1 = static_cast<double>(view[-1].l);
    double h2 = static_cast<double>(view[-2].h), l2 = static_cast<double>(view[-2].l);
    double v1 = static_cast<double>(view[-1].v), v2 = static_cast<double>(view[-2].v), v3 = static_cast<double>(view[-3].v);
    double v4 = static_cast<double>(view[-4].v), v5 = static_cast<double>(view[-5].v);

    switch (st.kind) {
        case F08_201_DELTA_ABSORP_DIV: {
            double p_diff = c - c4;
            bool es_bull = true, es_bear = true;
            if (has_distinct_ref(view, st.inst)) {
                double es_d = static_cast<double>(ref_bar(view, st.inst, 0).c) - static_cast<double>(ref_bar(view, st.inst, -4).c);
                double es_a = std::max(ref_atr14(view, st.inst, 0), 1e-4);
                es_bull = (es_d >= 0.0 || p_diff > 1.5 * cur_atr);
                es_bear = (es_d <= 0.0 || p_diff < -1.5 * cur_atr);
                (void)es_a;
            }
            double rng = h - l;
            if (rng > 1e-4 && v > 1.2 * sens * v5) {
                if (p_diff > 0.5 * cur_atr && (c - l) / rng > 0.60 && es_bull) {
                    bull = true; proposed_r = std::clamp(c - l, 3.5 * sc, 7.5 * sc);
                } else if (p_diff < -0.5 * cur_atr && (h - c) / rng > 0.60 && es_bear) {
                    bear = true; proposed_r = std::clamp(h - c, 3.5 * sc, 7.5 * sc);
                }
            }
            break;
        }
        case F08_201_DELTA_DIV_TRAP: {
            double p_diff = c - static_cast<double>(view[-10].c);
            double vol_acc = 0.0;
            for (int k = 0; k < 10; ++k) {
                const auto& bk = view[-k];
                vol_acc += (bk.c > bk.o ? 1.0 : -1.0) * static_cast<double>(bk.v);
            }
            double rng = h - l;
            if (rng > 1e-4) {
                if (p_diff < -0.8 * cur_atr && vol_acc > 0.0 && (c - l) / rng > 0.55) {
                    bull = true; proposed_r = std::clamp(c - l, 4.0 * sc, 9.0 * sc);
                } else if (p_diff > 0.8 * cur_atr && vol_acc < 0.0 && (h - c) / rng > 0.55) {
                    bear = true; proposed_r = std::clamp(h - c, 4.0 * sc, 9.0 * sc);
                }
            }
            break;
        }
        case F08_201_SWEEP_WICK_PIVOT: {
            double roll_h = -1e18, roll_l = 1e18;
            for (int k = 1; k <= 30; ++k) {
                roll_h = std::max(roll_h, static_cast<double>(view[-k].h));
                roll_l = std::min(roll_l, static_cast<double>(view[-k].l));
            }
            double rng = h - l;
            if (rng > 0.5 * cur_atr && v > 1.3 * sens * static_cast<double>(view[-10].v)) {
                double low_w = std::min(o, c) - l, hi_w = h - std::max(o, c);
                if (l < roll_l && c > roll_l && low_w / rng > 0.60) {
                    bull = true; proposed_r = std::clamp(c - l, 4.0 * sc, 12.0 * sc);
                } else if (h > roll_h && c < roll_h && hi_w / rng > 0.60) {
                    bear = true; proposed_r = std::clamp(h - c, 4.0 * sc, 12.0 * sc);
                }
            }
            break;
        }
        case F08_202_OVERNIGHT_FAIL_AUC: {
            if (!st.on_valid && (sess == 2 || sess == 3) && view.bar_index() >= 120) {
                double scan_h = -1e18, scan_l = 1e18;
                bool found_on = false;
                int32_t today = view.day_yyyymmdd();
                for (int k = 1; k <= 800 && static_cast<size_t>(k) <= view.bar_index(); ++k) {
                    int16_t em_k = 0; int32_t dt_k = 0;
                    u11::decode_et_and_cme_date(view[-k].ts_sec, em_k, dt_k);
                    if (dt_k != today) break;
                    int8_t sk = u11::classify_session(em_k, true);
                    if (sk == 0 || sk == 1) {
                        scan_h = std::max(scan_h, static_cast<double>(view[-k].h));
                        scan_l = std::min(scan_l, static_cast<double>(view[-k].l));
                        found_on = true;
                    }
                }
                if (found_on) { st.on_h = scan_h; st.on_l = scan_l; st.on_valid = true; }
            }
            if ((sess == 2 || sess == 3) && st.on_valid && st.sess_bars >= 10 && st.sess_bars <= 90) {
                if (l1 < st.on_l && c1 < st.on_l && c > st.on_l && c > o) {
                    bull = true; proposed_r = std::clamp(c - l1, 5.0 * sc, 14.0 * sc);
                } else if (h1 > st.on_h && c1 > st.on_h && c < st.on_h && c < o) {
                    bear = true; proposed_r = std::clamp(h1 - c, 5.0 * sc, 14.0 * sc);
                }
            }
            break;
        }
        case F08_202_POISSON_ARRIVAL: {
            double mv = 0.0, sv = 0.0;
            for (int k = 1; k <= 30; ++k) mv += static_cast<double>(view[-k].v);
            mv /= 30.0;
            for (int k = 1; k <= 30; ++k) {
                double d = static_cast<double>(view[-k].v) - mv;
                sv += d * d;
            }
            double sd_v = std::sqrt(sv / 30.0);
            if (v > mv + 2.8 * sens * sd_v && std::abs(c - o) > 1.4 * cur_atr) {
                bool es_ok = true;
                if (has_distinct_ref(view, st.inst)) {
                    double ec = static_cast<double>(ref_bar(view, st.inst, 0).c);
                    double eo = static_cast<double>(ref_bar(view, st.inst, 0).o);
                    es_ok = ((c > o && ec > eo) || (c < o && ec < eo));
                }
                if (es_ok) {
                    if (c > o) { bull = true; proposed_r = std::clamp(c - l, 3.8 * sc, 7.8 * sc); }
                    else { bear = true; proposed_r = std::clamp(h - c, 3.8 * sc, 7.8 * sc); }
                }
            }
            break;
        }
        case F08_203_WIENER_ENTROPY: {
            double sum_a = 0.0, sum_log = 0.0;
            for (int k = 0; k < 8; ++k) {
                double r = std::max(0.1 * sc, std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k].o)));
                sum_a += r;
                sum_log += std::log(r);
            }
            double flat = std::exp(sum_log / 8.0) / ((sum_a / 8.0) + 1e-6);
            if (flat < 0.32 * sens && std::abs(c - static_cast<double>(view[-8].c)) > 2.0 * cur_atr) {
                if (c > static_cast<double>(view[-8].c)) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_203_TRIPLE_INSIDE_COIL: {
            bool in1 = (h1 <= h2 && l1 >= l2);
            double h3 = static_cast<double>(view[-3].h), l3 = static_cast<double>(view[-3].l);
            bool in2 = (h2 <= h3 && l2 >= l3);
            if (in1 && in2 && (h3 - l3) < 1.2 * cur_atr && v > 1.5 * sens * v1) {
                if (c > h3) { bull = true; proposed_r = std::clamp(c - l3, 4.0 * sc, 12.0 * sc); }
                else if (c < l3) { bear = true; proposed_r = std::clamp(h3 - c, 4.0 * sc, 12.0 * sc); }
            }
            break;
        }
        case F08_204_CA_COINT_SPREAD: {
            if (!has_distinct_ref(view, st.inst)) return sig;
            double ec0 = std::max(1.0, static_cast<double>(ref_bar(view, st.inst, 0).c));
            double cur_sp = std::log(std::max(1.0, c) / ec0);
            double m_sp = 0.0;
            for (int k = 1; k <= 40; ++k) {
                double nck = std::max(1.0, static_cast<double>(view[-k].c));
                double eck = std::max(1.0, static_cast<double>(ref_bar(view, st.inst, -k).c));
                m_sp += std::log(nck / eck);
            }
            m_sp /= 40.0;
            double spread = cur_sp - m_sp;
            if (spread > 0.0035 * sens && c < o) {
                bear = true; proposed_r = std::clamp(h - c, 4.0 * sc, 8.5 * sc);
            } else if (spread < -0.0035 * sens && c > o) {
                bull = true; proposed_r = std::clamp(c - l, 4.0 * sc, 8.5 * sc);
            }
            break;
        }
        case F08_204_CA_LEADLAG_ACCEL: {
            if (!has_distinct_ref(view, st.inst)) return sig;
            double ec0 = static_cast<double>(ref_bar(view, st.inst, 0).c);
            double ec2 = static_cast<double>(ref_bar(view, st.inst, -2).c);
            double ec4 = static_cast<double>(ref_bar(view, st.inst, -4).c);
            double es_a = std::max(ref_atr14(view, st.inst, 0), 1e-4);
            double nq_acc = ((c - c2) - (c2 - c4)) / cur_atr;
            double es_acc = ((ec0 - ec2) - (ec2 - ec4)) / es_a;
            if (es_acc > 1.2 * sens && nq_acc < 0.2 * sens && c > o) {
                bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc);
            } else if (es_acc < -1.2 * sens && nq_acc > -0.2 * sens && c < o) {
                bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc);
            }
            break;
        }
        case F08_204_CA_VEL_DIV: {
            if (!has_distinct_ref(view, st.inst)) return sig;
            double ec0 = static_cast<double>(ref_bar(view, st.inst, 0).c);
            double ec5 = static_cast<double>(ref_bar(view, st.inst, -5).c);
            double es_a = std::max(ref_atr14(view, st.inst, 0), 1e-4);
            double nq_d = (c - c5) / cur_atr;
            double es_d = (ec0 - ec5) / es_a;
            if (es_d > 1.5 * sens && nq_d < 0.3 * sens && c > o) {
                bull = true; proposed_r = std::clamp(c - l2, 5.0 * sc, 12.0 * sc);
            } else if (es_d < -1.5 * sens && nq_d > -0.3 * sens && c < o) {
                bear = true; proposed_r = std::clamp(h2 - c, 5.0 * sc, 12.0 * sc);
            }
            break;
        }
        case F08_205_OB_ABSORP_SHELF: {
            double l3 = static_cast<double>(view[-3].l), h3 = static_cast<double>(view[-3].h);
            if (std::abs(l1 - l2) < 0.75 * sc && std::abs(l2 - l3) < 0.75 * sc && c > h1 && v > 1.2 * sens * v2) {
                bull = true; proposed_r = std::clamp(c - l1, 3.8 * sc, 7.5 * sc);
            } else if (std::abs(h1 - h2) < 0.75 * sc && std::abs(h2 - h3) < 0.75 * sc && c < l1 && v > 1.2 * sens * v2) {
                bear = true; proposed_r = std::clamp(h1 - c, 3.8 * sc, 7.5 * sc);
            }
            break;
        }
        case F08_206_FDI_PERSISTENCE: {
            double path_l = 0.0, hh = -1e18, ll = 1e18;
            for (int k = 0; k < 15; ++k) {
                path_l += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
                hh = std::max(hh, static_cast<double>(view[-k].h));
                ll = std::min(ll, static_cast<double>(view[-k].l));
            }
            double span = std::max(1e-4, hh - ll);
            double fdi = 1.0 + (std::log(std::max(1e-4, path_l / span)) / std::log(28.0));
            double d15 = c - static_cast<double>(view[-15].c);
            if (fdi < 1.22 * sens && std::abs(d15) > 2.2 * cur_atr) {
                if (d15 > 0.0) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_206_VWAP_3SIG_EXH: {
            double diff = c - vwap;
            double sd = std::max(view.session_vwap_std(0), 1.8 * cur_atr);
            if (diff < -2.6 * sens * sd && c > o) {
                bull = true; proposed_r = std::clamp(c - l, 5.0 * sc, 14.0 * sc);
            } else if (diff > 2.6 * sens * sd && c < o) {
                bear = true; proposed_r = std::clamp(h - c, 5.0 * sc, 14.0 * sc);
            }
            break;
        }
        case F08_207_OPEN_DRIVE_TRAP: {
            if (st.op_bars >= 5 && st.sess_bars >= 6 && st.sess_bars <= 15) {
                if (h1 > st.op_h && c < st.op_h && c < o) {
                    bear = true; proposed_r = std::clamp(h1 - c, 5.0 * sc, 14.0 * sc);
                } else if (l1 < st.op_l && c > st.op_l && c > o) {
                    bull = true; proposed_r = std::clamp(c - l1, 5.0 * sc, 14.0 * sc);
                }
            }
            break;
        }
        case F08_208_LOWVOL_VACUUM_GLIDE: {
            double vol_m = 0.0;
            for (int k = 1; k <= 15; ++k) vol_m += static_cast<double>(view[-k].v);
            vol_m /= 15.0;
            if (std::abs(c - c3) > 2.0 * cur_atr * sens && v < 0.70 * vol_m) {
                if (c > c3) { bull = true; proposed_r = std::clamp(c - l1, 4.0 * sc, 10.0 * sc); }
                else { bear = true; proposed_r = std::clamp(h1 - c, 4.0 * sc, 10.0 * sc); }
            }
            break;
        }
        case F08_209_BB_PINCH_EXPLODE: {
            double m10 = 0.0, v10 = 0.0;
            for (int k = 1; k <= 10; ++k) m10 += static_cast<double>(view[-k].c);
            m10 /= 10.0;
            for (int k = 1; k <= 10; ++k) {
                double d = static_cast<double>(view[-k].c) - m10;
                v10 += d * d;
            }
            double sd = std::sqrt(v10 / 10.0);
            if (sd < 0.35 * cur_atr && v > 1.8 * sens * v1) {
                if (c > m10 + 0.8 * cur_atr) { bull = true; proposed_r = std::clamp(c - (m10 - sd), 4.0 * sc, 12.0 * sc); }
                else if (c < m10 - 0.8 * cur_atr) { bear = true; proposed_r = std::clamp((m10 + sd) - c, 4.0 * sc, 12.0 * sc); }
            }
            break;
        }
        case F08_210_BLOCK_MOM_IGNIT: {
            double rng = h - l;
            double body = std::abs(c - o);
            if (rng > 1e-4 && v > 2.8 * sens * v5 && (body / rng) > 0.80 && body > 1.5 * cur_atr) {
                if (c > o) { bull = true; proposed_r = std::clamp(c - o, 5.0 * sc, 14.0 * sc); }
                else { bear = true; proposed_r = std::clamp(o - c, 5.0 * sc, 14.0 * sc); }
            }
            break;
        }
        case F08_211_WYCKOFF_SPRING: {
            double hh = -1e18, ll = 1e18;
            for (int k = 2; k <= 16; ++k) {
                hh = std::max(hh, static_cast<double>(view[-k].h));
                ll = std::min(ll, static_cast<double>(view[-k].l));
            }
            if ((hh - ll) < 2.5 * cur_atr) {
                if (l1 < ll && c > ll + 0.5 * cur_atr * sens && c > o) {
                    bull = true; proposed_r = std::clamp(c - l1, 4.0 * sc, 10.0 * sc);
                } else if (h1 > hh && c < hh - 0.5 * cur_atr * sens && c < o) {
                    bear = true; proposed_r = std::clamp(h1 - c, 4.0 * sc, 10.0 * sc);
                }
            }
            break;
        }
        case F08_212_KL_INFO_SHOCK: {
            double p1 = std::abs(c - o) / cur_atr + 1e-4;
            double p0 = std::abs(c1 - o1) / cur_atr + 1e-4;
            double kl = p1 * std::log(p1 / p0);
            if (kl > 2.8 * sens && v > 1.3 * v3) {
                if (c > o) { bull = true; proposed_r = std::clamp(c - l, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_212_MIDDAY_LUNCH_COIL: {
            if (em >= 780 && em <= 825) {
                double lhi = -1e18, llo = 1e18;
                for (int k = 1; k <= 40; ++k) {
                    lhi = std::max(lhi, static_cast<double>(view[-k].h));
                    llo = std::min(llo, static_cast<double>(view[-k].l));
                }
                if (c > lhi && c1 <= lhi) {
                    bull = true; proposed_r = std::clamp(c - 0.5 * (lhi + llo), 5.0 * sc, 14.0 * sc);
                } else if (c < llo && c1 >= llo) {
                    bear = true; proposed_r = std::clamp(0.5 * (lhi + llo) - c, 5.0 * sc, 14.0 * sc);
                }
            }
            break;
        }
        case F08_213_DOUBLE_EXH_WICK: {
            double r0 = h - l, r1 = h1 - l1;
            if (r0 > 0.5 * cur_atr && r1 > 0.5 * cur_atr) {
                double lw0 = (std::min(o, c) - l) / r0, lw1 = (std::min(o1, c1) - l1) / r1;
                double uw0 = (h - std::max(o, c)) / r0, uw1 = (h1 - std::max(o1, c1)) / r1;
                if (lw0 > 0.55 * sens && lw1 > 0.55 * sens && c > c1) {
                    bull = true; proposed_r = std::clamp(c - std::min(l, l1), 4.0 * sc, 12.0 * sc);
                } else if (uw0 > 0.55 * sens && uw1 > 0.55 * sens && c < c1) {
                    bear = true; proposed_r = std::clamp(std::max(h, h1) - c, 4.0 * sc, 12.0 * sc);
                }
            }
            break;
        }
        case F08_214_CVD_ICEBERG_TRAP: {
            double hh12 = -1e18, ll12 = 1e18;
            for (int k = 2; k <= 12; ++k) {
                hh12 = std::max(hh12, static_cast<double>(view[-k].h));
                ll12 = std::min(ll12, static_cast<double>(view[-k].l));
            }
            if (h1 > hh12 && c1 < (o1 + 0.3 * cur_atr) && c < l1 && v > 1.2 * sens * v2) {
                bear = true; proposed_r = std::clamp(h1 - c, 4.0 * sc, 8.0 * sc);
            } else if (l1 < ll12 && c1 > (o1 - 0.3 * cur_atr) && c > h1 && v > 1.2 * sens * v2) {
                bull = true; proposed_r = std::clamp(c - l1, 4.0 * sc, 8.0 * sc);
            }
            break;
        }
        case F08_214_KURTOSIS_FATTAIL: {
            double m = 0.0;
            for (int k = 0; k < 10; ++k) m += static_cast<double>(view[-k].c);
            m /= 10.0;
            double m2 = 0.0, m4 = 0.0;
            for (int k = 0; k < 10; ++k) {
                double d = static_cast<double>(view[-k].c) - m;
                double d2 = d * d;
                m2 += d2; m4 += d2 * d2;
            }
            m2 /= 10.0; m4 /= 10.0;
            double kurt = (m2 > 1e-6) ? (m4 / (m2 * m2)) : 0.0;
            if (kurt > 4.2 * sens && std::abs(c - m) > 1.3 * cur_atr) {
                if (c > m) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_214_MOC_1530_CASCADE: {
            if (em >= 930 && em <= 940 && v > 1.8 * sens * v5) {
                double phi = -1e18, plo = 1e18;
                for (int k = 1; k <= 20; ++k) {
                    phi = std::max(phi, static_cast<double>(view[-k].h));
                    plo = std::min(plo, static_cast<double>(view[-k].l));
                }
                if (c > phi) { bull = true; proposed_r = std::clamp(c - plo, 5.0 * sc, 14.0 * sc); }
                else if (c < plo) { bear = true; proposed_r = std::clamp(phi - c, 5.0 * sc, 14.0 * sc); }
            }
            break;
        }
        case F08_215_FVG_VACUUM_RETEST: {
            double h4 = static_cast<double>(view[-4].h), l4 = static_cast<double>(view[-4].l);
            if (l2 > h4 && c1 < l2 && c > l2 && c > o) {
                bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc);
            } else if (l4 > h2 && c1 > h2 && c < h2 && c < o) {
                bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc);
            }
            break;
        }
        case F08_216_DYN_ATR_SNAPBACK: {
            double ema20 = view.ema20(0);
            double stretch = (c - ema20) / cur_atr;
            if (stretch < -2.4 * sens && c > o) {
                bull = true; proposed_r = std::clamp(c - l, 4.0 * sc, 12.0 * sc);
            } else if (stretch > 2.4 * sens && c < o) {
                bear = true; proposed_r = std::clamp(h - c, 4.0 * sc, 12.0 * sc);
            }
            break;
        }
        case F08_216_HOLDER_SINGULARITY: {
            double d1 = std::max(1e-4, std::abs(c - c1));
            double d5 = std::max(1e-4, std::abs(c - c5));
            double holder = std::log(d1 / d5) / std::log(0.2);
            if (holder > 1.4 * sens && d1 > 1.2 * cur_atr) {
                if (c > c1) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_216_JUMPDIFF_SHOCK: {
            if (std::abs(c - o) / cur_atr > 1.6 * sens && v > 1.4 * v5) {
                if (c > o) { bull = true; proposed_r = std::clamp(c - l, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_217_NR7_TREND_EXP: {
            double r1 = h1 - l1;
            bool is_nr7 = true;
            for (int k = 2; k <= 7; ++k) {
                if ((static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l)) <= r1) {
                    is_nr7 = false; break;
                }
            }
            if (is_nr7) {
                double c20 = static_cast<double>(view[-20].c);
                if (c1 > c20 + 1.5 * cur_atr * sens && c > h1) {
                    bull = true; proposed_r = std::clamp(c - l1, 4.0 * sc, 10.0 * sc);
                } else if (c1 < c20 - 1.5 * cur_atr * sens && c < l1) {
                    bear = true; proposed_r = std::clamp(h1 - c, 4.0 * sc, 10.0 * sc);
                }
            }
            break;
        }
        case F08_218_MICRO_TREND_BRK: {
            double p1 = static_cast<double>(view[-15].c), p2 = static_cast<double>(view[-8].c), p3 = c2;
            if (p1 < p2 && p2 < p3 && c < p3 - 1.2 * cur_atr * sens && v > 1.3 * v2) {
                bear = true; proposed_r = std::clamp(p3 - c, 5.0 * sc, 12.0 * sc);
            } else if (p1 > p2 && p2 > p3 && c > p3 + 1.2 * cur_atr * sens && v > 1.3 * v2) {
                bull = true; proposed_r = std::clamp(c - p3, 5.0 * sc, 12.0 * sc);
            }
            break;
        }
        case F08_219_VWAP_REC_VEL:
        case F08_219_VWAP_REC_VOL: {
            bool is_vol = (st.kind == F08_219_VWAP_REC_VOL);
            double c4o = is_vol ? static_cast<double>(view[-5].c) : c4;
            double c8o = is_vol ? static_cast<double>(view[-10].c) : static_cast<double>(view[-8].c);
            double cross_m = is_vol ? 0.8 : 0.6;
            double vol_m = is_vol ? 1.7 : 1.35;
            if (c4o < vwap && c8o < vwap && c > vwap + cross_m * cur_atr && v > vol_m * sens * v5) {
                bull = true; proposed_r = std::clamp(c - vwap, 3.8 * sc, 10.0 * sc);
            } else if (c4o > vwap && c8o > vwap && c < vwap - cross_m * cur_atr && v > vol_m * sens * v5) {
                bear = true; proposed_r = std::clamp(vwap - c, 3.8 * sc, 10.0 * sc);
            }
            break;
        }
        case F08_220_RUNLEN_COMPLEXITY: {
            int runs = 1;
            for (int k = 1; k < 8; ++k) {
                bool u0 = static_cast<double>(view[-k].c) > static_cast<double>(view[-k].o);
                bool u1 = static_cast<double>(view[-k + 1].c) > static_cast<double>(view[-k + 1].o);
                if (u0 != u1) runs++;
            }
            double d8 = c - static_cast<double>(view[-8].c);
            if (runs <= 2 && std::abs(d8) > 1.8 * cur_atr * sens) {
                if (d8 > 0.0) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.5 * sc); }
                else { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_222_MINKOWSKI_CONE: {
            double dp = std::abs(c - c5);
            double c_spd = 0.35 * cur_atr;
            double ds2 = dp * dp - (c_spd * 5.0) * (c_spd * 5.0);
            bool es_ok = true;
            if (has_distinct_ref(view, st.inst)) {
                double ec0 = static_cast<double>(ref_bar(view, st.inst, 0).c);
                double ec5 = static_cast<double>(ref_bar(view, st.inst, -5).c);
                es_ok = ((c > c5 && ec0 > ec5) || (c < c5 && ec0 < ec5));
            }
            if (ds2 > 0.0 && dp > 1.8 * cur_atr * sens && v > 1.3 * v4 && es_ok) {
                if (c > c5 && c > vwap) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.2 * sc); }
                else if (c < c5 && c < vwap) { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.2 * sc); }
            }
            break;
        }
        case F08_224_ERGODIC_FLOW: {
            double t_sum = 0.0;
            for (int k = 1; k <= 20; ++k) {
                t_sum += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
            }
            double t_mean = t_sum / 20.0;
            double ens_var = 0.0;
            for (int k = 1; k <= 20; ++k) {
                double d = std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c)) - t_mean;
                ens_var += d * d;
            }
            ens_var = std::sqrt(ens_var / 20.0);
            if (std::abs(c - c3) > 2.2 * ens_var * sens && std::abs(c - c3) > 1.4 * cur_atr && v > 1.4 * v3) {
                if (c > c3 && c > vwap) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.0 * sc); }
                else if (c < c3 && c < vwap) { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.0 * sc); }
            }
            break;
        }
        case F08_225_HAWKES_EXCIT: {
            if (!has_distinct_ref(view, st.inst)) return sig;
            double ec0 = static_cast<double>(ref_bar(view, st.inst, 0).c);
            double ec2 = static_cast<double>(ref_bar(view, st.inst, -2).c);
            double es_a = std::max(ref_atr14(view, st.inst, 0), 1e-4);
            double j_nq = c - c2, j_es = ec0 - ec2;
            if (std::abs(j_nq) > 1.3 * cur_atr * sens && std::abs(j_es) > 0.4 * es_a) {
                if (j_nq > 0.0 && j_es > 0.0 && c > vwap) {
                    bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.2 * sc);
                } else if (j_nq < 0.0 && j_es < 0.0 && c < vwap) {
                    bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.2 * sc);
                }
            }
            break;
        }
        case F08_229_QUEUING_OVERFLOW: {
            double arr = v / std::max(0.25 * sc, h - l);
            double avg_s = 0.0;
            for (int k = 1; k <= 10; ++k) {
                double rk = std::max(0.25 * sc, static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l));
                avg_s += static_cast<double>(view[-k].v) / rk;
            }
            avg_s /= 10.0;
            if (avg_s > 0.0 && (arr / avg_s) > 2.2 * sens && std::abs(c - c3) > 1.5 * cur_atr) {
                if (c > c3 && c > vwap) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.0 * sc); }
                else if (c < c3 && c < vwap) { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.0 * sc); }
            }
            break;
        }
        case F08_230_PERCOLATION_HWY: {
            int p_sites = 0;
            for (int k = 0; k < 6; ++k) {
                if (static_cast<double>(view[-k].v) > static_cast<double>(view[-k - 1].v)) p_sites++;
            }
            double d6 = c - static_cast<double>(view[-6].c);
            if (p_sites >= 4 && std::abs(d6) > 2.2 * cur_atr * sens) {
                if (d6 > 0.0 && c > vwap) { bull = true; proposed_r = std::clamp(c - l2, 4.0 * sc, 7.5 * sc); }
                else if (d6 < 0.0 && c < vwap) { bear = true; proposed_r = std::clamp(h2 - c, 4.0 * sc, 7.5 * sc); }
            }
            break;
        }
        case F08_233_VANDERPOL_RUPTURE: {
            double r_avg = 0.0;
            for (int k = 2; k <= 8; ++k) {
                r_avg += static_cast<double>(view[-k].h) - static_cast<double>(view[-k].l);
            }
            r_avg /= 7.0;
            if (r_avg > 0.0 && (h - l) / r_avg > 2.2 * sens && std::abs(c - o) > 1.4 * cur_atr && v > 1.4 * v3) {
                if (c > o && c > vwap) { bull = true; proposed_r = std::clamp(c - l, 3.8 * sc, 7.0 * sc); }
                else if (c < o && c < vwap) { bear = true; proposed_r = std::clamp(h - c, 3.8 * sc, 7.0 * sc); }
            }
            break;
        }
        case F08_235_PARETO_LEVY: {
            double avg_j = 0.0;
            for (int k = 2; k <= 14; ++k) {
                avg_j += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 2].c));
            }
            avg_j /= 13.0;
            double j0 = std::abs(c - c2);
            if (avg_j > 0.0 && j0 > 2.6 * avg_j * sens && v > 1.4 * v3) {
                if (c > c2 && c > vwap) { bull = true; proposed_r = std::clamp(c - l2, 3.8 * sc, 7.0 * sc); }
                else if (c < c2 && c < vwap) { bear = true; proposed_r = std::clamp(h2 - c, 3.8 * sc, 7.0 * sc); }
            }
            break;
        }
        default: break;
    }

    if (bull || bear) {
        int8_t dir = bull ? 1 : -1;
        double sl = proposed_r * st.p2;
        double tp = sl * target_r;
        double r_trig = (st.p3 > 0.0) ? (sl * st.p3) : 0.0;
        double r_trail = (st.p4 > 0.0) ? (sl * st.p4) : 0.0;
        st.trades_today++;
        return make_sig(dir, 0, 0.0, sl, tp, r_trig, r_trail, 0, 0, st.max_hold, 2, st.inst);
    }
    return sig;
}

template <F08Kind K>
inline void add_f08(std::vector<s546::RegisteredStrategy>& out,
                    const std::string& uid, const std::string& name,
                    const std::string& src, uint8_t sess_mask, bool cross_asset,
                    std::vector<u11::ParamSpec> params) {
    s546::RegisteredStrategy r;
    r.uid = uid;
    r.display_name = name;
    r.super_family = "08_AGY_Kimi_Chat_Archived_Historical_CPP_Strategies";
    r.source_note = src;
    r.fidelity = "EXACT_SOURCE";
    r.needs_cross_asset = cross_asset;
    r.session_mask = sess_mask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<F08State>(uid, f08_on_bar, f08_init<K>);
    out.push_back(std::move(r));
}

inline std::vector<u11::ParamSpec> f08_fisher_params(double ft, double mr, double sl, double tp, double s2_t, double s2_l, int mh) {
    return {
        p_lin("fisher_thresh", ft, 2.0e5, 8.0e5),
        p_lin("mean_r_thresh", mr, 5.0e-5, 2.5e-4),
        p_lin("sl_atr", sl, 0.7, 2.0),
        p_lin("tp_atr", tp, 1.5, 4.5),
        p_lin("stage2_trig_atr", s2_t, 0.8, 2.5),
        p_lin("stage2_lock_atr", s2_l, 0.2, 1.2),
        p_int("max_hold", mh, 20, 90)
    };
}

inline std::vector<u11::ParamSpec> f08_kalman_params(double tp, double sl, double exp_r) {
    return {
        p_lin("q_kal", 0.015, 0.005, 0.05),
        p_lin("res_atr", 2.0, 1.2, 3.0),
        p_lin("sl_atr", sl, 0.7, 2.0),
        p_lin("tp_atr", tp, 1.5, 4.0),
        p_lin("atr_exp_ratio", exp_r, 0.75, 1.25),
        p_int("max_hold", 45, 20, 90)
    };
}

inline std::vector<u11::ParamSpec> f08_mad_params(double zt, double sl, double tp) {
    return {
        p_int("mad_win", 30, 15, 60),
        p_lin("z_thresh", zt, 1.6, 3.2),
        p_lin("min_mad_atr", 0.4, 0.2, 0.8),
        p_lin("sl_atr", sl, 0.7, 2.0),
        p_lin("tp_atr", tp, 1.2, 3.5)
    };
}

inline std::vector<u11::ParamSpec> f08_b10_params(double rr, int mh = 90) {
    return {
        p_lin("target_rr", rr, 1.5, 25.0),
        p_lin("thresh_scale", 1.0, 0.6, 1.5),
        p_lin("sl_scale", 1.0, 0.7, 1.6),
        p_lin("ratchet_trig_r", 0.0, 0.0, 4.0),
        p_lin("ratchet_dist_r", 0.0, 0.0, 2.0),
        p_int("max_hold", mh, 30, 180)
    };
}

inline void register_f08(std::vector<s546::RegisteredStrategy>& out) {
    add_f08<F08_CASPER_IFVG_5M>(out, "chat_arch_casper_ifvg_5m", "Casper 5m Inverted FVG (Optimal 2.0pt / 22.5 SL / 35 TP)", "validate_reel29_and_casper_ifvg_sop.py", 0xC, false,
        {p_lin("fvg_min_pts", 2.0, 1.0, 6.0), p_lin("sl_pts", 22.5, 10.0, 40.0), p_lin("tp_pts", 35.0, 20.0, 60.0), p_bool("stop_on_win", true), p_int("max_trades", 3, 1, 8)});

    const std::string l2792 = "line2792_alpha_optimizer_nq.cpp";
    const std::string l2798 = "line2798_champions_verified_200k.cpp";
    const std::string l2788 = "line2788_champions_200k_validator.cpp";
    const std::string l2886 = "line2886_champions_full_quant_suite.cpp";

    add_f08<F08_FISHER_BASE_ALL_1C>(out, "chat_arch_001_fisher_baseline_allsessions_1contract", "1_Fisher_Baseline_AllSessions_1Contract", l2792, 0xF, false, f08_fisher_params(5.0e5, 1.0e-4, 1.2, 2.5, 0.0, 0.0, 40));
    add_f08<F08_FISHER_INFO_DYN_NQ>(out, "chat_arch_001_fisher_info_dynamic_nq_1m", "1_Fisher_Info_Dynamic_NQ_1m", l2798, 0xD, false, f08_fisher_params(5.0e5, 1.0e-4, 1.2, 2.5, 0.0, 0.0, 45));
    add_f08<F08_FISHER_TWOSTAGE_NQ>(out, "chat_arch_001_fisher_twostage_nq", "1_Fisher_TwoStage_NQ", l2788, 0xD, false, f08_fisher_params(4.5e5, 1.0e-4, 1.1, 3.2, 1.8, 0.6, 45));
    add_f08<F08_FISHER_INFO_VOLT_NQ>(out, "chat_arch_002_fisher_info_voltarget_nq_1m", "2_Fisher_Info_VolTarget_NQ_1m", l2798, 0xD, false, f08_fisher_params(5.0e5, 1.0e-4, 1.2, 2.5, 0.0, 0.0, 45));
    add_f08<F08_FISHER_SKIPLDN_1C>(out, "chat_arch_002_fisher_skiplondon_asian_ny_1contract", "2_Fisher_SkipLondon_Asian_NY_1Contract", l2792, 0xD, false, f08_fisher_params(5.0e5, 1.0e-4, 1.2, 2.5, 0.0, 0.0, 40));
    add_f08<F08_KALMAN_REGIME_NQ>(out, "chat_arch_003_kalman_regime_gated_nq", "3_Kalman_Regime_Gated_NQ", l2788, 0xF, false, f08_kalman_params(2.8, 1.0, 0.95));
    add_f08<F08_TOKYO_NY_FLOW_NQ>(out, "chat_arch_003_tokyo_ny_imbalance_flow_nq_1m", "3_Tokyo_NY_Imbalance_Flow_NQ_1m", l2798, 0x5, false, {p_int("lookback", 20, 10, 40), p_lin("ret_atr", 1.5, 0.8, 2.5), p_lin("sl_atr", 1.1, 0.7, 2.0), p_lin("tp_atr", 3.0, 1.5, 4.5)});
    add_f08<F08_KALMAN_LATENT_VEL_NQ>(out, "chat_arch_004_kalman_latent_velocity_nq_1m", "4_Kalman_Latent_Velocity_NQ_1m", l2798, 0xF, false, f08_kalman_params(2.5, 1.0, 0.0));
    add_f08<F08_ZSCORE_MAD_DYN_NQ>(out, "chat_arch_004_zscore_mad_dynamic_target_nq", "4_ZScore_MAD_Dynamic_Target_NQ", l2788, 0xD, false, f08_mad_params(2.4, 1.2, 2.2));
    add_f08<F08_FISHER_NY_HIGHCONV_1C>(out, "chat_arch_005_fisher_ny_only_highconviction_1contract", "5_Fisher_NY_Only_HighConviction_1Contract", l2792, 0xC, false, f08_fisher_params(4.0e5, 1.2e-4, 1.1, 2.8, 0.0, 0.0, 45));
    add_f08<F08_ZSCORE_MAD_INST_ES>(out, "chat_arch_005_zscore_mad_institutional_es_1m", "5_ZScore_MAD_Institutional_ES_1m", l2798, 0xF, false, f08_mad_params(2.2, 1.0, 1.8));
    add_f08<F08_FISHER_KALMAN_ASNY_1C>(out, "chat_arch_006_fisher_kalman_confluence_asian_ny_1contract", "6_Fisher_Kalman_Confluence_Asian_NY_1Contract", l2792, 0xD, false, f08_fisher_params(4.5e5, 1.0e-4, 1.1, 2.8, 0.0, 0.0, 45));
    add_f08<F08_KALMAN_HIGHBETA_YM>(out, "chat_arch_006_kalman_highbeta_drift_ym_1m", "6_Kalman_HighBeta_Drift_YM_1m", l2798, 0xF, false, f08_kalman_params(2.5, 1.0, 0.0));
    add_f08<F08_FISHER_SKIPLDN_VOLT_2C>(out, "chat_arch_007_fisher_skiplondon_voltarget_2contractcap", "7_Fisher_SkipLondon_VolTarget_2ContractCap", l2792, 0xD, false, f08_fisher_params(5.0e5, 1.0e-4, 1.2, 2.5, 0.0, 0.0, 40));
    add_f08<F08_SETTLEMENT_TOKYO_REV_NQ>(out, "chat_arch_007_settlement_tokyo_reversion_nq", "7_Settlement_Tokyo_Reversion_NQ", l2788, 0x1, false, {p_int("lookback", 15, 8, 30), p_lin("ret_atr", 2.2, 1.2, 3.5), p_lin("sl_atr", 1.2, 0.8, 2.2), p_lin("tp_atr", 2.6, 1.5, 4.0)});
    add_f08<F08_FISHER_TWOSTAGE_ES>(out, "chat_arch_008_fisher_twostage_es", "8_Fisher_TwoStage_ES", l2788, 0xD, false, f08_fisher_params(5.0e5, 1.0e-4, 1.1, 2.8, 1.5, 0.5, 45));
    add_f08<F08_FISHER_KALMAN_VOLT_2C>(out, "chat_arch_009_fisher_kalman_confluence_voltarget_2cap", "9_Fisher_Kalman_Confluence_VolTarget_2Cap", l2792, 0xD, false, f08_fisher_params(4.5e5, 1.0e-4, 1.1, 2.8, 0.0, 0.0, 45));
    add_f08<F08_ZSCORE_MAD_DYN_ES>(out, "chat_arch_009_zscore_mad_dynamic_target_es", "9_ZScore_MAD_Dynamic_Target_ES", l2788, 0xF, false, f08_mad_params(2.5, 1.2, 2.2));
    add_f08<F08_KALMAN_REGIME_ES>(out, "chat_arch_010_kalman_regime_gated_es", "10_Kalman_Regime_Gated_ES", l2788, 0xF, false, f08_kalman_params(2.6, 1.1, 0.0));
    add_f08<F08_FISHER_TWOSTAGE_YM>(out, "chat_arch_011_fisher_twostage_ym", "11_Fisher_TwoStage_YM", l2788, 0xD, false, f08_fisher_params(4.5e5, 1.0e-4, 1.1, 2.8, 1.5, 0.5, 45));
    add_f08<F08_KALMAN_REGIME_YM>(out, "chat_arch_012_kalman_regime_gated_ym", "12_Kalman_Regime_Gated_YM", l2788, 0xF, false, f08_kalman_params(2.6, 1.1, 0.0));
    add_f08<F08_ZSCORE_MAD_DYN_YM>(out, "chat_arch_013_zscore_mad_dynamic_target_ym", "13_ZScore_MAD_Dynamic_Target_YM", l2788, 0xF, false, f08_mad_params(2.5, 1.2, 2.2));
    add_f08<F08_FISHER_INFO_METRIC_NQ_ALL>(out, "chat_arch_023_fisher_info_metric_nq_all", "23_Fisher_Info_Metric_NQ_All", l2886, 0xF, false, f08_fisher_params(5.0e5, 1.0e-4, 1.05, 2.85, 0.0, 0.0, 45));

    const std::string l5914 = "line5914_test_batch10_all20.cpp";
    const std::string l5932 = "line5932_test_pure_7rr.cpp";
    const std::string l5948 = "line5948_test_pure_7rr.cpp";
    const std::string tnov  = "test_20_novel_math_strats.cpp";
    const std::string tb11  = "test_batch11_ultra_high_rr.cpp";

    add_f08<F08_201_DELTA_ABSORP_DIV>(out, "chat_arch_201_delta_absorption_divergence", "201_Delta_Absorption_Divergence", l5948, 0xF, true, f08_b10_params(7.5));
    add_f08<F08_201_DELTA_DIV_TRAP>(out, "chat_arch_201_delta_divergence_trap", "201_Delta_Divergence_Trap", l5932, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_201_SWEEP_WICK_PIVOT>(out, "chat_arch_201_sweep_wick_pivot_reversal", "201_Sweep_Wick_Pivot_Reversal", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_202_OVERNIGHT_FAIL_AUC>(out, "chat_arch_202_overnight_failed_auction_trap", "202_Overnight_Failed_Auction_Trap", l5914, 0xC, false, f08_b10_params(7.5));
    add_f08<F08_202_POISSON_ARRIVAL>(out, "chat_arch_202_poisson_arrival_intensity_surge", "202_Poisson_Arrival_Intensity_Surge", l5948, 0xF, true, f08_b10_params(7.5));
    add_f08<F08_203_WIENER_ENTROPY>(out, "chat_arch_203_spectral_wiener_entropy_collapse", "203_Spectral_Wiener_Entropy_Collapse", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_203_TRIPLE_INSIDE_COIL>(out, "chat_arch_203_triple_inside_coil_expansion", "203_Triple_Inside_Coil_Expansion", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_204_CA_COINT_SPREAD>(out, "chat_arch_204_crossasset_cointegration_spread", "204_CrossAsset_Cointegration_Spread", l5932, 0xF, true, f08_b10_params(7.5));
    add_f08<F08_204_CA_LEADLAG_ACCEL>(out, "chat_arch_204_crossasset_leadlag_acceleration", "204_CrossAsset_LeadLag_Acceleration", l5948, 0xF, true, f08_b10_params(7.5));
    add_f08<F08_204_CA_VEL_DIV>(out, "chat_arch_204_crossasset_velocity_divergence", "204_CrossAsset_Velocity_Divergence", l5914, 0xF, true, f08_b10_params(7.5));
    add_f08<F08_205_OB_ABSORP_SHELF>(out, "chat_arch_205_orderbook_absorption_shelf", "205_OrderBook_Absorption_Shelf", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_206_FDI_PERSISTENCE>(out, "chat_arch_206_fractal_dimension_persistence", "206_Fractal_Dimension_Persistence", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_206_VWAP_3SIG_EXH>(out, "chat_arch_206_vwap_3sigma_exhaustion_reversion", "206_VWAP_3Sigma_Exhaustion_Reversion", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_207_OPEN_DRIVE_TRAP>(out, "chat_arch_207_open_drive_trap_engulf", "207_Open_Drive_Trap_Engulf", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_208_LOWVOL_VACUUM_GLIDE>(out, "chat_arch_208_lowvolume_vacuum_glide", "208_LowVolume_Vacuum_Glide", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_209_BB_PINCH_EXPLODE>(out, "chat_arch_209_bollinger_pinch_volatility_explode", "209_Bollinger_Pinch_Volatility_Explode", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_210_BLOCK_MOM_IGNIT>(out, "chat_arch_210_block_momentum_ignition", "210_Block_Momentum_Ignition", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_211_WYCKOFF_SPRING>(out, "chat_arch_211_wyckoff_spring_range_reentry", "211_Wyckoff_Spring_Range_Reentry", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_212_KL_INFO_SHOCK>(out, "chat_arch_212_kl_information_gain_shock", "212_KL_Information_Gain_Shock", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_212_MIDDAY_LUNCH_COIL>(out, "chat_arch_212_midday_lunch_coil_breakout", "212_Midday_Lunch_Coil_Breakout", l5914, 0x8, false, f08_b10_params(7.5));
    add_f08<F08_213_DOUBLE_EXH_WICK>(out, "chat_arch_213_double_exhaustion_wick_cluster", "213_Double_Exhaustion_Wick_Cluster", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_214_CVD_ICEBERG_TRAP>(out, "chat_arch_214_cvd_iceberg_absorption_trapping", "214_CVD_Iceberg_Absorption_Trapping", tnov, 0xF, false, f08_b10_params(2.2));
    add_f08<F08_214_KURTOSIS_FATTAIL>(out, "chat_arch_214_kurtosis_fattail_liquidation", "214_Kurtosis_FatTail_Liquidation", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_214_MOC_1530_CASCADE>(out, "chat_arch_214_moc_1530_imbalance_cascade", "214_MOC_1530_Imbalance_Cascade", l5914, 0x8, false, f08_b10_params(7.5));
    add_f08<F08_215_FVG_VACUUM_RETEST>(out, "chat_arch_215_fvg_vacuum_retest_rejection", "215_FVG_Vacuum_Retest_Rejection", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_216_DYN_ATR_SNAPBACK>(out, "chat_arch_216_dynamic_atr_envelope_snapback", "216_Dynamic_ATR_Envelope_Snapback", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_216_HOLDER_SINGULARITY>(out, "chat_arch_216_holder_continuity_singularity", "216_Holder_Continuity_Singularity", l5932, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_216_JUMPDIFF_SHOCK>(out, "chat_arch_216_jumpdiffusion_volatility_shock", "216_JumpDiffusion_Volatility_Shock", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_217_NR7_TREND_EXP>(out, "chat_arch_217_nr7_trend_expansion", "217_NR7_Trend_Expansion", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_218_MICRO_TREND_BRK>(out, "chat_arch_218_micro_trendline_cascade_break", "218_Micro_Trendline_Cascade_Break", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_219_VWAP_REC_VEL>(out, "chat_arch_219_vwap_reclaim_velocity_surge", "219_VWAP_Reclaim_Velocity_Surge", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_219_VWAP_REC_VOL>(out, "chat_arch_219_vwap_reclaim_volume_surge", "219_VWAP_Reclaim_Volume_Surge", l5914, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_220_RUNLEN_COMPLEXITY>(out, "chat_arch_220_runlength_complexity_deficit", "220_RunLength_Complexity_Deficit", l5948, 0xF, false, f08_b10_params(7.5));
    add_f08<F08_222_MINKOWSKI_CONE>(out, "chat_arch_222_minkowski_cone_escape", "222_Minkowski_Cone_Escape", tb11, 0xF, true, f08_b10_params(20.0, 120));
    add_f08<F08_224_ERGODIC_FLOW>(out, "chat_arch_224_ergodic_flow_anomaly", "224_Ergodic_Flow_Anomaly", tb11, 0xF, false, f08_b10_params(20.0, 120));
    add_f08<F08_225_HAWKES_EXCIT>(out, "chat_arch_225_hawkes_excitation_cascade", "225_Hawkes_Excitation_Cascade", tb11, 0xF, true, f08_b10_params(20.0, 120));
    add_f08<F08_229_QUEUING_OVERFLOW>(out, "chat_arch_229_queuing_server_overflow", "229_Queuing_Server_Overflow", tb11, 0xF, false, f08_b10_params(20.0, 120));
    add_f08<F08_230_PERCOLATION_HWY>(out, "chat_arch_230_percolation_highway", "230_Percolation_Highway", tb11, 0xF, false, f08_b10_params(20.0, 120));
    add_f08<F08_233_VANDERPOL_RUPTURE>(out, "chat_arch_233_van_der_pol_rupture", "233_VanDerPol_Rupture", tb11, 0xF, false, f08_b10_params(20.0, 120));
    add_f08<F08_235_PARETO_LEVY>(out, "chat_arch_235_pareto_levy_flight", "235_Pareto_Levy_Flight", tb11, 0xF, false, f08_b10_params(20.0, 120));
}


// ============================================================================
// FAMILY 11: 11_Ten_Quant_Strats_and_Alpha_Champions_CPP (29 UIDs)
// Sources: champion_quad_engine.cpp, grand_champion_trio_engine.cpp,
//          matrix_12leg_decoupled_engine.cpp, alpha_champions_test.cpp (16),
//          ten_quant_strats_engine.cpp + master_100_multiverse_sweep.cpp (10)
// ============================================================================

enum F11Kind {
    // Core C++ Production Multi-Leg Engines (3)
    F11_CHAMPION_QUAD = 0,
    F11_GRAND_CHAMPION_TRIO,
    F11_MATRIX_12LEG_DECOUPLED,
    // Alpha Champions 01..16 (16)
    F11_AC01_FISHER_CHAND_NQ,
    F11_AC02_FISHER_NY_SURGE_NQ,
    F11_AC03_FISHER_5M_AGG_NQ,
    F11_AC04_KALMAN_CHAND_NQ,
    F11_AC05_DFA_RUNNER_NQ,
    F11_AC06_ZSCORE_ASYM_NQ,
    F11_AC07_VOL_VACUUM_LVN_NQ,
    F11_AC08_MTF_5M_1M_FRACTAL_NQ,
    F11_AC09_TOKYO_NY_DUAL_NQ,
    F11_AC10_SETTLE_MOC_ASYM_NQ,
    F11_AC11_FISHER_CHAND_ES,
    F11_AC12_KALMAN_CHAND_ES,
    F11_AC13_ZSCORE_ASYM_ES,
    F11_AC14_FISHER_CHAND_YM,
    F11_AC15_KALMAN_CHAND_YM,
    F11_AC16_ZSCORE_ASYM_YM,
    // Ten Quant Workspace 0..9 (10)
    F11_TQ0_KER_REGIME,
    F11_TQ1_LEADLAG_PULLBACK,
    F11_TQ2_LDNNY_OPEN_RESET,
    F11_TQ3_MOC_INST_DRIFT,
    F11_TQ4_AMT_VALUE_AREA,
    F11_TQ5_TURTLE_SOUP_ORB,
    F11_TQ6_VSA_ABSORPTION,
    F11_TQ7_BB_KC_SQUEEZE,
    F11_TQ8_MULTIANCHOR_VWAP_MR,
    F11_TQ9_DALTON_IB_EXT
};

struct F11State {
    F11Kind kind = F11_CHAMPION_QUAD;
    u11::InstrumentSpec inst{};
    double p0 = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0, p6 = 0.0, p7 = 0.0;
    int max_hold = 60;
    int max_trades_day = 6;

    int last_day = -1;
    int trades_today = 0;
    int last_sess = -99;
    int trades_sess = 0;

    // Causal Kalman state (AC04, AC12, AC15)
    bool kal_init = false;
    double kal_x = 0.0;
    double kal_p = 1.0;

    // Squeeze counter (TQ7)
    int squeeze_cnt = 0;

    // Cached London Value Area (TQ4)
    int ldn_va_day = -1;
    bool ldn_va_ready = false;
    double ldn_val = 0.0, ldn_poc = 0.0, ldn_vah = 0.0;

    // Cached London H/L (TQ2)
    int ldn_hl_day = -1;
    double ldn_hi = -1e18, ldn_lo = 1e18;
    bool ldn_hl_ready = false;

    // Cached 13:00 ET anchor close (TQ3)
    int moc_day = -1;
    double moc_anchor = 0.0;
    bool moc_anchor_ready = false;
};

template <F11Kind K>
inline void f11_init(F11State& st, const std::vector<double>& params, const u11::InstrumentSpec* spec) {
    st = F11State{};
    st.kind = K;
    if (spec) st.inst = *spec;
    switch (K) {
        case F11_CHAMPION_QUAD:
            st.p0 = p_at(params, 0, 1.50);   // hydraulic_threshold
            st.p1 = p_at(params, 1, 0.80);   // tsallis_cutoff
            st.p2 = p_at(params, 2, 3.00);   // volterra_cutoff
            st.p3 = p_at(params, 3, 1.50);   // copula_cutoff
            st.p4 = p_at(params, 4, 36.0);   // sl_pts
            st.p5 = p_at(params, 5, 60.0);   // tp_pts
            st.p6 = p_at(params, 6, 1.50);   // ratchet_trigger
            st.p7 = p_at(params, 7, 0.75);   // ratchet_trail
            st.max_hold = 90;
            st.max_trades_day = 8;
            break;
        case F11_GRAND_CHAMPION_TRIO:
            st.p0 = p_at(params, 0, 30.0);   // or_mins
            st.p1 = p_at(params, 1, 1.50);   // hydraulic_threshold
            st.p2 = p_at(params, 2, 0.80);   // tsallis_cutoff
            st.p3 = p_at(params, 3, 3.00);   // volterra_cutoff
            st.p4 = p_at(params, 4, 36.25);  // sl_pts
            st.p5 = p_at(params, 5, 60.0);   // tp_pts
            st.p6 = p_at(params, 6, 1.50);   // ratchet_trigger
            st.p7 = p_at(params, 7, 0.75);   // ratchet_trail
            st.max_hold = 90;
            st.max_trades_day = 8;
            break;
        case F11_MATRIX_12LEG_DECOUPLED:
            st.p0 = p_at(params, 0, 5.0);    // asian_sl
            st.p1 = p_at(params, 1, 6.5);    // london_sl
            st.p2 = p_at(params, 2, 8.0);    // ny_sl
            st.p3 = p_at(params, 3, 60.0);   // target_pts
            st.p4 = p_at(params, 4, 1.0);    // ratchet_trig_scale
            st.p5 = p_at(params, 5, 0.5);    // ratchet_trail_pts
            st.p6 = p_at(params, 6, 1.50);   // pulse/copula th
            st.p7 = p_at(params, 7, 0.80);   // tsallis cutoff
            st.max_hold = 90;
            st.max_trades_day = 12;
            break;
        case F11_AC01_FISHER_CHAND_NQ:
        case F11_AC11_FISHER_CHAND_ES:
        case F11_AC14_FISHER_CHAND_YM:
            st.p0 = p_at(params, 0, 4.5e5);
            st.p1 = p_at(params, 1, 1.2e-4);
            st.p2 = p_at(params, 2, 1.2);    // sl_atr
            st.p3 = p_at(params, 3, 2.0);    // chand_trig_atr
            st.p4 = p_at(params, 4, 1.5);    // chand_trail_atr
            st.max_hold = static_cast<int>(std::round(p_at(params, 5, 60.0)));
            break;
        case F11_AC02_FISHER_NY_SURGE_NQ:
            st.p0 = p_at(params, 0, 3.0e5);
            st.p1 = p_at(params, 1, 1.5e-4);
            st.p2 = p_at(params, 2, 1.0);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = static_cast<int>(std::round(p_at(params, 5, 60.0)));
            break;
        case F11_AC03_FISHER_5M_AGG_NQ:
            st.p0 = p_at(params, 0, 2.0e5);
            st.p1 = p_at(params, 1, 2.5e-4);
            st.p2 = p_at(params, 2, 1.5);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = static_cast<int>(std::round(p_at(params, 5, 60.0)));
            break;
        case F11_AC04_KALMAN_CHAND_NQ:
        case F11_AC12_KALMAN_CHAND_ES:
        case F11_AC15_KALMAN_CHAND_YM:
            st.p0 = p_at(params, 0, 0.02);
            st.p1 = p_at(params, 1, 2.2);
            st.p2 = p_at(params, 2, 1.2);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = static_cast<int>(std::round(p_at(params, 5, 60.0)));
            break;
        case F11_AC05_DFA_RUNNER_NQ:
            st.p0 = p_at(params, 0, 30.0);
            st.p1 = p_at(params, 1, 2.5);
            st.p2 = p_at(params, 2, 1.1);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = 60;
            break;
        case F11_AC06_ZSCORE_ASYM_NQ:
        case F11_AC13_ZSCORE_ASYM_ES:
        case F11_AC16_ZSCORE_ASYM_YM:
            st.p0 = p_at(params, 0, 30.0);
            st.p1 = p_at(params, 1, 2.8);
            st.p2 = p_at(params, 2, 1.0);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = 60;
            break;
        case F11_AC07_VOL_VACUUM_LVN_NQ:
            st.p0 = p_at(params, 0, 1.8);
            st.p1 = p_at(params, 1, 2.0);
            st.p2 = p_at(params, 2, 1.0);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = 60;
            break;
        case F11_AC08_MTF_5M_1M_FRACTAL_NQ:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 0.2);
            st.p2 = p_at(params, 2, 1.1);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = 60;
            break;
        case F11_AC09_TOKYO_NY_DUAL_NQ:
            st.p0 = p_at(params, 0, 10.0);
            st.p1 = p_at(params, 1, 1.8);
            st.p2 = p_at(params, 2, 1.2);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = 60;
            break;
        case F11_AC10_SETTLE_MOC_ASYM_NQ:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 2.0);
            st.p2 = p_at(params, 2, 1.2);
            st.p3 = p_at(params, 3, 2.0);
            st.p4 = p_at(params, 4, 1.5);
            st.max_hold = 60;
            break;
        case F11_TQ0_KER_REGIME:
            st.p0 = p_at(params, 0, 30.0);
            st.p1 = p_at(params, 1, 0.55);
            st.p2 = p_at(params, 2, 0.25);
            st.p3 = p_at(params, 3, 1.6);
            st.p4 = p_at(params, 4, 3.2);
            st.max_hold = 90;
            st.max_trades_day = 6;
            break;
        case F11_TQ1_LEADLAG_PULLBACK:
            st.p0 = p_at(params, 0, 60.0);
            st.p1 = p_at(params, 1, 20.0);
            st.p2 = p_at(params, 2, 1.5);
            st.p3 = p_at(params, 3, 2.5);
            st.p4 = p_at(params, 4, 0.0); // 1 = RSI mode
            st.max_hold = 90;
            st.max_trades_day = 4;
            break;
        case F11_TQ2_LDNNY_OPEN_RESET:
            st.p0 = p_at(params, 0, 0.10);
            st.p1 = p_at(params, 1, 0.25);
            st.max_hold = 90;
            st.max_trades_day = 2;
            break;
        case F11_TQ3_MOC_INST_DRIFT:
            st.p0 = p_at(params, 0, 1.0);
            st.p1 = p_at(params, 1, 1.8);
            st.max_hold = 40;
            st.max_trades_day = 1;
            break;
        case F11_TQ4_AMT_VALUE_AREA:
            st.p0 = p_at(params, 0, 0.15);
            st.p1 = p_at(params, 1, 0.25);
            st.p2 = p_at(params, 2, 0.0); // tp mode: 0=POC, 1=opp VA
            st.max_hold = 90;
            st.max_trades_day = 3;
            break;
        case F11_TQ5_TURTLE_SOUP_ORB:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 0.15);
            st.p2 = p_at(params, 2, 1.2);
            st.p3 = p_at(params, 3, 0.20);
            st.max_hold = 90;
            st.max_trades_day = 3;
            break;
        case F11_TQ6_VSA_ABSORPTION:
            st.p0 = p_at(params, 0, 2.0);
            st.p1 = p_at(params, 1, 0.50);
            st.p2 = p_at(params, 2, 0.20);
            st.p3 = p_at(params, 3, 2.5);
            st.max_hold = 90;
            st.max_trades_day = 4;
            break;
        case F11_TQ7_BB_KC_SQUEEZE:
            st.p0 = p_at(params, 0, 20.0);
            st.p1 = p_at(params, 1, 2.0);
            st.p2 = p_at(params, 2, 1.5);
            st.p3 = p_at(params, 3, 5.0);
            st.p4 = p_at(params, 4, 2.5);
            st.max_hold = 90;
            st.max_trades_day = 4;
            break;
        case F11_TQ8_MULTIANCHOR_VWAP_MR:
            st.p0 = p_at(params, 0, 2.1);
            st.p1 = p_at(params, 1, 1.2);
            st.max_hold = 90;
            st.max_trades_day = 3;
            break;
        case F11_TQ9_DALTON_IB_EXT:
            st.p0 = p_at(params, 0, 1.6);
            st.p1 = p_at(params, 1, 2.5);
            st.p2 = p_at(params, 2, 1.2);
            st.p3 = p_at(params, 3, 0.4);
            st.max_hold = 90;
            st.max_trades_day = 3;
            break;
    }
}

inline u11::PluginSignal f11_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto& st = *static_cast<F11State*>(state_ptr);
    u11::PluginSignal sig{};

    if (view.day_yyyymmdd() != st.last_day) {
        st.last_day = view.day_yyyymmdd();
        st.trades_today = 0;
        st.squeeze_cnt = 0;
    }
    int sess = view.session_id();
    if (sess != st.last_sess) {
        st.last_sess = sess;
        st.trades_sess = 0;
        st.squeeze_cnt = 0;
    }

    if (view.bar_index() < 65) return sig;
    const auto& b = view[0];
    double c = static_cast<double>(b.c);
    double v = std::max(100.0, static_cast<double>(b.v));
    double cur_atr = std::max(view.atr14(0), 2.0 * st.inst.tick_size);
    double pt_s = sym_pt_scale(st.inst);
    int em = view.et_mins();

    // Update causal Kalman continuously on lag -1
    if (st.kind == F11_AC04_KALMAN_CHAND_NQ || st.kind == F11_AC12_KALMAN_CHAND_ES || st.kind == F11_AC15_KALMAN_CHAND_YM) {
        double c1 = static_cast<double>(view[-1].c);
        if (!st.kal_init) { st.kal_x = c1; st.kal_p = 1.0; st.kal_init = true; }
        double r_kal = std::max(1e-4, cur_atr * cur_atr);
        st.kal_p += st.p0;
        double kg = st.kal_p / (st.kal_p + r_kal);
        st.kal_x += kg * (c1 - st.kal_x);
        st.kal_p = (1.0 - kg) * st.kal_p;
    }

    // Update BB/KC squeeze state continuously during active bars
    double sma20 = 0.0, std20 = 0.0, kc_up = 0.0, kc_dn = 0.0;
    bool was_in_sq = false;
    if (st.kind == F11_TQ7_BB_KC_SQUEEZE) {
        int win = std::clamp(static_cast<int>(std::round(st.p0)), 10, 40);
        for (int k = 1; k <= win; ++k) sma20 += static_cast<double>(view[-k].c);
        sma20 /= static_cast<double>(win);
        double sq_sum = 0.0;
        for (int k = 1; k <= win; ++k) {
            double d = static_cast<double>(view[-k].c) - sma20;
            sq_sum += d * d;
        }
        std20 = std::sqrt(sq_sum / static_cast<double>(win));
        double bb_up = sma20 + st.p1 * std20;
        double bb_dn = sma20 - st.p1 * std20;
        kc_up = sma20 + st.p2 * cur_atr;
        kc_dn = sma20 - st.p2 * cur_atr;
        if (bb_up < kc_up && bb_dn > kc_dn) {
            st.squeeze_cnt++;
            was_in_sq = true;
        }
    }

    if (view.in_position || st.trades_today >= st.max_trades_day) {
        if (st.kind == F11_TQ7_BB_KC_SQUEEZE && !was_in_sq) st.squeeze_cnt = 0;
        return sig;
    }

    // ------------------------------------------------------------------
    // 1) Core Production Multi-Leg Engines (Quad, Trio, Matrix 12-Leg)
    // ------------------------------------------------------------------
    if (st.kind == F11_CHAMPION_QUAD || st.kind == F11_GRAND_CHAMPION_TRIO || st.kind == F11_MATRIX_12LEG_DECOUPLED) {
        if (!view.or_formed(15)) return sig;
        double c1 = static_cast<double>(view[-1].c);
        double c2 = static_cast<double>(view[-2].c);
        double pulse = (c - c2) * v / ((cur_atr + 1e-4) * 1000.0);
        double p1 = std::abs(c - c1) / (cur_atr + 1e-4) + 1e-4;
        double p2 = std::abs(c1 - c2) / (cur_atr + 1e-4) + 1e-4;
        double norm = p1 + p2; p1 /= norm; p2 /= norm;
        double tsallis_s = (1.0 - (std::pow(p1, 1.5) + std::pow(p2, 1.5))) / 0.5;
        double volterra = 0.0;
        for (int k = 1; k <= 8; ++k) {
            volterra += std::pow(static_cast<double>(k), -0.4) * (static_cast<double>(view[-k + 1].c) - static_cast<double>(view[-k].c));
        }
        double u_mom = std::clamp(std::abs(c - c1) / (cur_atr + 1e-4), 0.001, 0.999);
        double v_vol = std::clamp(v / 1000.0, 0.001, 0.999);
        double c_val = std::pow(std::pow(u_mom, -2.0) + std::pow(v_vol, -2.0) - 1.0, -0.5);
        double copula = (c_val / (u_mom * v_vol)) - 1.0;

        if (st.kind == F11_GRAND_CHAMPION_TRIO) {
            int om = (st.p0 >= 45) ? 60 : (st.p0 >= 30 ? 30 : 15);
            if (!view.or_formed(om)) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double sl = st.p4 * pt_s, tp = st.p5 * pt_s;
            double rt = st.p6 * pt_s, rd = st.p7 * pt_s;
            if (c > oh && (pulse >= st.p1 || (tsallis_s <= st.p2 && c > c1) || volterra >= st.p3)) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, rt, rd, 0, 0, st.max_hold, 2, st.inst);
            }
            if (c < ol && (pulse <= -st.p1 || (tsallis_s <= st.p2 && c < c1) || volterra <= -st.p3)) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, rt, rd, 0, 0, st.max_hold, 2, st.inst);
            }
            return sig;
        }

        double sl = 0.0, tp = 0.0, rt = 0.0, rd = 0.0;
        double hyd_th = (st.kind == F11_CHAMPION_QUAD) ? st.p0 : st.p6;
        double tsa_cut = (st.kind == F11_CHAMPION_QUAD) ? st.p1 : st.p7;
        double volt_cut = (st.kind == F11_CHAMPION_QUAD) ? st.p2 : 3.0;
        double cop_cut = (st.kind == F11_CHAMPION_QUAD) ? st.p3 : st.p6;
        if (st.kind == F11_CHAMPION_QUAD) {
            sl = st.p4 * pt_s; tp = st.p5 * pt_s; rt = st.p6 * pt_s; rd = st.p7 * pt_s;
        } else {
            double raw_sl = (sess == 0) ? st.p0 : ((sess == 1) ? st.p1 : st.p2);
            double raw_rt = (sess == 0) ? 0.75 : ((sess == 1) ? 1.00 : 1.25);
            sl = raw_sl * pt_s;
            tp = st.p3 * pt_s;
            rt = raw_rt * st.p4 * pt_s;
            rd = st.p5 * pt_s;
        }
        int8_t dir = 0;
        if (view.or_formed(30)) {
            if (c > view.or_high(30) && pulse >= hyd_th) dir = 1;
            else if (c < view.or_low(30) && pulse <= -hyd_th) dir = -1;
        }
        if (dir == 0 && view.or_formed(15)) {
            if (c > view.or_high(15) && tsallis_s <= tsa_cut && c > c1) dir = 1;
            else if (c < view.or_low(15) && tsallis_s <= tsa_cut && c < c1) dir = -1;
        }
        if (dir == 0 && view.or_formed(60)) {
            if (c > view.or_high(60) && volterra >= volt_cut) dir = 1;
            else if (c < view.or_low(60) && volterra <= -volt_cut) dir = -1;
        }
        if (dir == 0 && view.or_formed(15)) {
            if (c > view.or_high(15) && copula >= cop_cut) dir = 1;
            else if (c < view.or_low(15) && copula >= cop_cut) dir = -1;
        }
        if (dir != 0) {
            st.trades_today++;
            return make_sig(dir, 0, 0.0, sl, tp, rt, rd, 0, 0, st.max_hold, 2, st.inst);
        }
        return sig;
    }

    // ------------------------------------------------------------------
    // 2) Alpha Champions 01..16 (alpha_champions_test.cpp)
    // ------------------------------------------------------------------
    if (st.kind >= F11_AC01_FISHER_CHAND_NQ && st.kind <= F11_AC16_ZSCORE_ASYM_YM) {
        int8_t dir = 0;
        switch (st.kind) {
            case F11_AC01_FISHER_CHAND_NQ:
            case F11_AC11_FISHER_CHAND_ES:
            case F11_AC14_FISHER_CHAND_YM: {
                double sr = 0.0, s2 = 0.0;
                for (int k = 1; k <= 20; ++k) {
                    double r = (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c)) / std::max(1.0, static_cast<double>(view[-k - 1].c));
                    sr += r; s2 += r * r;
                }
                double mr = sr / 20.0, vr = (s2 / 20.0) - mr * mr;
                if (vr > 1e-9 && (1.0 / vr) > st.p0 && std::abs(mr) > st.p1) dir = (mr > 0.0) ? 1 : -1;
                break;
            }
            case F11_AC02_FISHER_NY_SURGE_NQ: {
                if ((sess == 2 || sess == 3) && em >= 585 && em <= 930) {
                    double sr = 0.0, s2 = 0.0;
                    for (int k = 1; k <= 15; ++k) {
                        double r = (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c)) / std::max(1.0, static_cast<double>(view[-k - 1].c));
                        sr += r; s2 += r * r;
                    }
                    double mr = sr / 15.0, vr = (s2 / 15.0) - mr * mr;
                    if (vr > 1e-9 && (1.0 / vr) > st.p0 && std::abs(mr) > st.p1) dir = (mr > 0.0) ? 1 : -1;
                }
                break;
            }
            case F11_AC03_FISHER_5M_AGG_NQ: {
                if (view.bar_index() % 5 == 0) {
                    double sr = 0.0, s2 = 0.0;
                    for (int k = 5; k <= 50; k += 5) {
                        double r = (static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 5].c)) / std::max(1.0, static_cast<double>(view[-k - 5].c));
                        sr += r; s2 += r * r;
                    }
                    double mr = sr / 10.0, vr = (s2 / 10.0) - mr * mr;
                    if (vr > 1e-9 && (1.0 / vr) > st.p0 && std::abs(mr) > st.p1) dir = (mr > 0.0) ? 1 : -1;
                }
                break;
            }
            case F11_AC04_KALMAN_CHAND_NQ:
            case F11_AC12_KALMAN_CHAND_ES:
            case F11_AC15_KALMAN_CHAND_YM: {
                double res = static_cast<double>(view[-1].c) - st.kal_x;
                if (std::abs(res) > st.p1 * cur_atr) dir = (res > 0.0) ? 1 : -1;
                break;
            }
            case F11_AC05_DFA_RUNNER_NQ: {
                int w = std::clamp(static_cast<int>(std::round(st.p0)), 15, 50);
                double base_c = static_cast<double>(view[-w - 1].c);
                double sy = 0.0;
                for (int k = 1; k <= w; ++k) sy += (static_cast<double>(view[-k].c) - base_c);
                double my = sy / static_cast<double>(w);
                double fl = 0.0;
                for (int k = 1; k <= w; ++k) {
                    double dev = (static_cast<double>(view[-k].c) - base_c) - my;
                    fl += dev * dev;
                }
                fl = std::sqrt(fl / static_cast<double>(w));
                if (fl > st.p1 * cur_atr) dir = (static_cast<double>(view[-1].c) > static_cast<double>(view[-w].c)) ? 1 : -1;
                break;
            }
            case F11_AC06_ZSCORE_ASYM_NQ:
            case F11_AC13_ZSCORE_ASYM_ES:
            case F11_AC16_ZSCORE_ASYM_YM: {
                int w = std::clamp(static_cast<int>(std::round(st.p0)), 15, 60);
                double med = static_cast<double>(view[-(w / 2)].c);
                double mad = 0.0;
                for (int k = 1; k <= w; ++k) mad += std::abs(static_cast<double>(view[-k].c) - med);
                mad /= static_cast<double>(w);
                if (mad > 0.5 * cur_atr) {
                    double z = (static_cast<double>(view[-1].c) - med) / mad;
                    if (z > st.p1) dir = 1;
                    else if (z < -st.p1) dir = -1;
                }
                break;
            }
            case F11_AC07_VOL_VACUUM_LVN_NQ: {
                double vc = static_cast<double>(view[-1].v), va = 0.0;
                for (int k = 1; k <= 20; ++k) va += static_cast<double>(view[-k].v);
                va /= 20.0;
                double vel = std::abs(static_cast<double>(view[-1].c) - static_cast<double>(view[-5].c));
                if (vel > st.p0 * cur_atr && vc > st.p1 * va) {
                    dir = (static_cast<double>(view[-1].c) > static_cast<double>(view[-5].c)) ? 1 : -1;
                }
                break;
            }
            case F11_AC08_MTF_5M_1M_FRACTAL_NQ: {
                int lb = std::clamp(static_cast<int>(std::round(st.p0)), 8, 30);
                double hh = -1e18, ll = 1e18;
                for (int k = 1; k <= lb; ++k) {
                    hh = std::max(hh, static_cast<double>(view[-k].h));
                    ll = std::min(ll, static_cast<double>(view[-k].l));
                }
                double c1 = static_cast<double>(view[-1].c), c2 = static_cast<double>(view[-2].c);
                if (c1 > hh - st.p1 * cur_atr && c1 > c2) dir = 1;
                else if (c1 < ll + st.p1 * cur_atr && c1 < c2) dir = -1;
                break;
            }
            case F11_AC09_TOKYO_NY_DUAL_NQ: {
                bool vt = (em >= 1215 && em <= 1290) || (em >= 15 && em <= 90) || (em >= 575 && em <= 660);
                if (vt) {
                    int lb = std::clamp(static_cast<int>(std::round(st.p0)), 5, 25);
                    double mv = static_cast<double>(view[-1].c) - static_cast<double>(view[-lb].c);
                    if (mv > st.p1 * cur_atr) dir = 1;
                    else if (mv < -st.p1 * cur_atr) dir = -1;
                }
                break;
            }
            case F11_AC10_SETTLE_MOC_ASYM_NQ: {
                bool vt = (em >= 1200 && em <= 1245) || (em >= 0 && em <= 45);
                if (vt) {
                    int lb = std::clamp(static_cast<int>(std::round(st.p0)), 8, 30);
                    double ret = static_cast<double>(view[-1].c) - static_cast<double>(view[-lb].c);
                    if (ret < -st.p1 * cur_atr) dir = 1;
                    else if (ret > st.p1 * cur_atr) dir = -1;
                }
                break;
            }
            default: break;
        }
        if (dir != 0) {
            st.trades_today++;
            double sl = st.p2 * cur_atr;
            return make_sig(dir, 0, 0.0, sl, 0.0, st.p3 * cur_atr, st.p4 * cur_atr, 0, 0, st.max_hold, 2, st.inst);
        }
        return sig;
    }

    // ------------------------------------------------------------------
    // 3) Ten Quant Strats Workspace 0..9
    // ------------------------------------------------------------------
    double p_c = static_cast<double>(view[-1].c);
    double p_h = static_cast<double>(view[-1].h), p_l = static_cast<double>(view[-1].l), p_o = static_cast<double>(view[-1].o);

    switch (st.kind) {
        case F11_TQ0_KER_REGIME: {
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 15, 50);
            double net_chg = std::abs(p_c - static_cast<double>(view[-lb].c));
            double gross = 0.0, hh = -1e18, ll = 1e18;
            for (int k = 1; k <= lb; ++k) {
                hh = std::max(hh, static_cast<double>(view[-k].h));
                ll = std::min(ll, static_cast<double>(view[-k].l));
                if (k < lb) gross += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c));
            }
            double er = (gross > 1e-6) ? (net_chg / gross) : 0.0;
            if (er > st.p1) {
                if (p_c > hh - 0.05 * cur_atr) {
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, st.p3 * cur_atr, st.p4 * cur_atr, 0, 0, 1.2 * cur_atr, 0.2 * cur_atr, st.max_hold, 3, st.inst);
                }
                if (p_c < ll + 0.05 * cur_atr) {
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, st.p3 * cur_atr, st.p4 * cur_atr, 0, 0, 1.2 * cur_atr, 0.2 * cur_atr, st.max_hold, 3, st.inst);
                }
            } else if (er < st.p2) {
                double mid = 0.5 * (hh + ll);
                double tp_dist = std::max(cur_atr, std::abs(c - mid));
                if (p_c >= hh - 0.05 * cur_atr) {
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, 0.9 * cur_atr, tp_dist, 0, 0, 1.2 * cur_atr, 0.2 * cur_atr, st.max_hold, 3, st.inst);
                }
                if (p_c <= ll + 0.05 * cur_atr) {
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, 0.9 * cur_atr, tp_dist, 0, 0, 1.2 * cur_atr, 0.2 * cur_atr, st.max_hold, 3, st.inst);
                }
            }
            break;
        }
        case F11_TQ1_LEADLAG_PULLBACK: {
            if (sess != 2 && sess != 3) return sig;
            int trend_lb = std::clamp(static_cast<int>(std::round(st.p0)), 20, 64);
            int fast_lb = std::clamp(static_cast<int>(std::round(st.p1)), 5, 30);
            double sma_t = 0.0, sma_f = 0.0;
            for (int k = 1; k <= trend_lb; ++k) sma_t += static_cast<double>(view[-k].c);
            sma_t /= static_cast<double>(trend_lb);
            for (int k = 1; k <= fast_lb; ++k) sma_f += static_cast<double>(view[-k].c);
            sma_f /= static_cast<double>(fast_lb);
            double sl = st.p2 * cur_atr, tp = st.p3 * sl;
            if (st.p4 > 0.5) {
                double gain = 0.0, loss = 0.0;
                for (int k = 1; k <= 14; ++k) {
                    double chg = static_cast<double>(view[-k].c) - static_cast<double>(view[-k - 1].c);
                    if (chg > 0) gain += chg; else loss += std::abs(chg);
                }
                double rs = (loss > 1e-4) ? (gain / loss) : 10.0;
                double rsi = 100.0 - (100.0 / (1.0 + rs));
                if (p_c > sma_t && rsi < 40.0) { st.trades_today++; return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst); }
                if (p_c < sma_t && rsi > 60.0) { st.trades_today++; return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst); }
            } else {
                if (p_c > sma_t && p_c <= sma_f + 0.1 * cur_atr && p_c >= sma_f - 0.5 * cur_atr) {
                    st.trades_today++; return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                }
                if (p_c < sma_t && p_c >= sma_f - 0.1 * cur_atr && p_c <= sma_f + 0.5 * cur_atr) {
                    st.trades_today++; return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                }
            }
            break;
        }
        case F11_TQ2_LDNNY_OPEN_RESET: {
            if (sess != 2 || em > 660) return sig;
            if (st.ldn_hl_day != view.day_yyyymmdd()) {
                st.ldn_hl_day = view.day_yyyymmdd();
                st.ldn_hl_ready = scan_et_window_hl(view, 180, 570, 500, st.ldn_hi, st.ldn_lo);
            }
            if (!st.ldn_hl_ready || st.ldn_hi <= st.ldn_lo) return sig;
            double mid = 0.5 * (st.ldn_hi + st.ldn_lo);
            if (p_h > st.ldn_hi + st.p0 * cur_atr && p_c < st.ldn_hi) {
                double sl = std::max(cur_atr, (p_h + st.p1 * cur_atr) - c);
                double tp = std::max(cur_atr, c - mid);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            if (p_l < st.ldn_lo - st.p0 * cur_atr && p_c > st.ldn_lo) {
                double sl = std::max(cur_atr, c - (p_l - st.p1 * cur_atr));
                double tp = std::max(cur_atr, mid - c);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F11_TQ3_MOC_INST_DRIFT: {
            if (em < 915 || em > 925) return sig;
            if (st.moc_day != view.day_yyyymmdd()) {
                st.moc_day = view.day_yyyymmdd();
                st.moc_anchor_ready = scan_et_anchor_close(view, 780, 220, st.moc_anchor);
            }
            if (!st.moc_anchor_ready) return sig;
            if (p_c > st.moc_anchor + st.p0 * cur_atr) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, st.p1 * cur_atr, 0.0, 0, 0, 0, 0, st.max_hold, 10, st.inst);
            }
            if (p_c < st.moc_anchor - st.p0 * cur_atr) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, st.p1 * cur_atr, 0.0, 0, 0, 0, 0, st.max_hold, 10, st.inst);
            }
            break;
        }
        case F11_TQ4_AMT_VALUE_AREA: {
            if ((sess != 2 && sess != 3) || em > 780) return sig;
            if (st.ldn_va_day != view.day_yyyymmdd()) {
                st.ldn_va_day = view.day_yyyymmdd();
                st.ldn_va_ready = scan_et_quantile_va(view, 180, 570, 500, st.ldn_val, st.ldn_poc, st.ldn_vah);
            }
            if (!st.ldn_va_ready || st.ldn_vah <= st.ldn_val) return sig;
            double tgt_s = (st.p2 > 0.5) ? st.ldn_val : st.ldn_poc;
            double tgt_l = (st.p2 > 0.5) ? st.ldn_vah : st.ldn_poc;
            if (p_h > st.ldn_vah + st.p0 * cur_atr && p_c < st.ldn_vah) {
                double sl = std::max(cur_atr, (p_h + st.p1 * cur_atr) - c);
                double tp = std::max(cur_atr, c - tgt_s);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            if (p_l < st.ldn_val - st.p0 * cur_atr && p_c > st.ldn_val) {
                double sl = std::max(cur_atr, c - (p_l - st.p1 * cur_atr));
                double tp = std::max(cur_atr, tgt_l - c);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F11_TQ5_TURTLE_SOUP_ORB: {
            int om = (st.p0 >= 25) ? 30 : 15;
            if ((sess != 2 && sess != 3) || !view.or_formed(om) || em > 720) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double omid = 0.5 * (oh + ol);
            if (p_h > oh + st.p1 * cur_atr && p_h < oh + st.p2 * cur_atr && p_c < oh) {
                double sl = std::max(cur_atr, (p_h + st.p3 * cur_atr) - c);
                double tp = std::max(cur_atr, c - omid);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            if (p_l < ol - st.p1 * cur_atr && p_l > ol - st.p2 * cur_atr && p_c > ol) {
                double sl = std::max(cur_atr, c - (p_l - st.p3 * cur_atr));
                double tp = std::max(cur_atr, omid - c);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            break;
        }
        case F11_TQ6_VSA_ABSORPTION: {
            double v_avg = 0.0;
            for (int k = 2; k <= 21; ++k) v_avg += static_cast<double>(view[-k].v);
            v_avg /= 20.0;
            double rng = p_h - p_l;
            if (v_avg > 0.0 && rng > 0.3 * cur_atr && static_cast<double>(view[-1].v) >= st.p0 * v_avg) {
                double up_w = p_h - std::max(p_o, p_c);
                double dn_w = std::min(p_o, p_c) - p_l;
                if (up_w >= st.p1 * rng) {
                    double sl = std::max(cur_atr, (p_h + st.p2 * cur_atr) - c);
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                }
                if (dn_w >= st.p1 * rng) {
                    double sl = std::max(cur_atr, c - (p_l - st.p2 * cur_atr));
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                }
            }
            break;
        }
        case F11_TQ7_BB_KC_SQUEEZE: {
            if (!was_in_sq) {
                int min_sq = std::clamp(static_cast<int>(std::round(st.p3)), 2, 15);
                int prev_sq = st.squeeze_cnt;
                st.squeeze_cnt = 0;
                if (prev_sq >= min_sq) {
                    if (p_c > kc_up) {
                        double sl = std::max(0.5 * cur_atr, c - sma20);
                        st.trades_today++;
                        return make_sig(1, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                    }
                    if (p_c < kc_dn) {
                        double sl = std::max(0.5 * cur_atr, sma20 - c);
                        st.trades_today++;
                        return make_sig(-1, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                    }
                }
            }
            break;
        }
        case F11_TQ8_MULTIANCHOR_VWAP_MR: {
            if (em < 630 || em > 840) return sig;
            double vwap = view.session_vwap(0);
            double sd = view.session_vwap_std(0);
            if (sd <= 1.0 * st.inst.tick_size) return sig;
            if (p_c >= vwap + st.p0 * sd) {
                double sl = std::max(cur_atr, st.p1 * sd);
                double tp = std::max(cur_atr, c - vwap);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            if (p_c <= vwap - st.p0 * sd) {
                double sl = std::max(cur_atr, st.p1 * sd);
                double tp = std::max(cur_atr, vwap - c);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            break;
        }
        case F11_TQ9_DALTON_IB_EXT: {
            if ((sess != 2 && sess != 3) || !view.or_formed(60) || em > 840) return sig;
            double ib_h = view.or_high(60), ib_l = view.or_low(60);
            double ib_r = ib_h - ib_l, ib_mid = 0.5 * (ib_h + ib_l);
            if (ib_r <= 2.0 * st.inst.tick_size) return sig;
            if (ib_r < st.p0 * cur_atr) {
                if (p_c > ib_h) {
                    double sl = std::max(cur_atr, c - ib_mid);
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, std::max(cur_atr, st.p2 * ib_r), 0, 0, 0, 0, st.max_hold, 4, st.inst);
                }
                if (p_c < ib_l) {
                    double sl = std::max(cur_atr, ib_mid - c);
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, std::max(cur_atr, st.p2 * ib_r), 0, 0, 0, 0, st.max_hold, 4, st.inst);
                }
            } else if (ib_r > st.p1 * cur_atr) {
                if (p_c >= ib_h - 0.2 * cur_atr) {
                    double sl = std::max(cur_atr, (ib_h + st.p3 * cur_atr) - c);
                    double tp = std::max(cur_atr, c - ib_mid);
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
                }
                if (p_c <= ib_l + 0.2 * cur_atr) {
                    double sl = std::max(cur_atr, c - (ib_l - st.p3 * cur_atr));
                    double tp = std::max(cur_atr, ib_mid - c);
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
                }
            }
            break;
        }
        default: break;
    }
    return sig;
}

template <F11Kind K>
inline void add_f11(std::vector<s546::RegisteredStrategy>& out,
                    const std::string& uid, const std::string& name,
                    const std::string& src, uint8_t sess_mask,
                    std::vector<u11::ParamSpec> params) {
    s546::RegisteredStrategy r;
    r.uid = uid;
    r.display_name = name;
    r.super_family = "11_Ten_Quant_Strats_and_Alpha_Champions_CPP";
    r.source_note = src;
    r.fidelity = "EXACT_SOURCE";
    r.needs_cross_asset = false;
    r.session_mask = sess_mask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<F11State>(uid, f11_on_bar, f11_init<K>);
    out.push_back(std::move(r));
}

inline std::vector<u11::ParamSpec> f11_chand_fisher_params(double ft, double mr, double sl) {
    return {
        p_lin("fisher_thresh", ft, 1.5e5, 8.0e5),
        p_lin("mean_r_thresh", mr, 5.0e-5, 3.5e-4),
        p_lin("sl_atr", sl, 0.7, 2.0),
        p_lin("chand_trig_atr", 2.0, 1.0, 3.5),
        p_lin("chand_trail_atr", 1.5, 0.8, 2.5),
        p_int("max_hold", 60, 25, 120)
    };
}

inline void register_f11(std::vector<s546::RegisteredStrategy>& out) {
    add_f11<F11_CHAMPION_QUAD>(out, "cpp_engines_champion_quad", "Champion Quad Engine (S159+S151+S157+S155)", "champion_quad_engine.cpp", 0xF,
        {p_lin("hydraulic_th", 1.5, 0.8, 2.5), p_lin("tsallis_cut", 0.8, 0.4, 1.0), p_lin("volterra_cut", 3.0, 1.5, 5.0), p_lin("copula_cut", 1.5, 0.8, 3.0),
         p_lin("sl_pts", 36.0, 15.0, 55.0), p_lin("tp_pts", 60.0, 30.0, 95.0), p_lin("ratchet_trig", 1.5, 0.5, 5.0), p_lin("ratchet_trail", 0.75, 0.25, 3.0)});
    add_f11<F11_GRAND_CHAMPION_TRIO>(out, "cpp_engines_grand_champion_trio", "Grand Champion Trio Engine (S159+S151+S157)", "grand_champion_trio_engine.cpp", 0xF,
        {p_int("or_mins", 30, 15, 60, 3), p_lin("hydraulic_th", 1.5, 0.8, 2.5), p_lin("tsallis_cut", 0.8, 0.4, 1.0), p_lin("volterra_cut", 3.0, 1.5, 5.0),
         p_lin("sl_pts", 36.25, 15.0, 55.0), p_lin("tp_pts", 60.0, 30.0, 95.0), p_lin("ratchet_trig", 1.5, 0.5, 5.0), p_lin("ratchet_trail", 0.75, 0.25, 3.0)});
    add_f11<F11_MATRIX_12LEG_DECOUPLED>(out, "cpp_engines_matrix_12leg_decoupled", "Matrix 12-Leg Decoupled Engine (Config #31396)", "matrix_12leg_decoupled_engine.cpp", 0x7,
        {p_lin("asian_sl", 5.0, 3.0, 15.0), p_lin("london_sl", 6.5, 4.0, 20.0), p_lin("ny_sl", 8.0, 4.0, 25.0), p_lin("tp_pts", 60.0, 20.0, 90.0),
         p_lin("ratchet_scale", 1.0, 0.5, 3.0), p_lin("ratchet_trail", 0.5, 0.25, 2.0), p_lin("pulse_copula_th", 1.5, 0.8, 2.5), p_lin("tsallis_cut", 0.8, 0.4, 1.0)});

    const std::string act = "alpha_champions_test.cpp";
    add_f11<F11_AC01_FISHER_CHAND_NQ>(out, "engine_cpp_alpha_champion_01_1_fisher_chandelier_trail_nq", "Alpha Champion 01: Fisher Chandelier Trail NQ", act, 0xF, f11_chand_fisher_params(4.5e5, 1.2e-4, 1.2));
    add_f11<F11_AC02_FISHER_NY_SURGE_NQ>(out, "engine_cpp_alpha_champion_02_2_fisher_pure_ny_surge_nq", "Alpha Champion 02: Fisher Pure NY Surge NQ", act, 0xC, f11_chand_fisher_params(3.0e5, 1.5e-4, 1.0));
    add_f11<F11_AC03_FISHER_5M_AGG_NQ>(out, "engine_cpp_alpha_champion_03_3_fisher_5min_aggregated_nq", "Alpha Champion 03: Fisher 5-Min Aggregated NQ", act, 0xF, f11_chand_fisher_params(2.0e5, 2.5e-4, 1.5));
    add_f11<F11_AC04_KALMAN_CHAND_NQ>(out, "engine_cpp_alpha_champion_04_4_kalman_chandelier_trail_nq", "Alpha Champion 04: Kalman Chandelier Trail NQ", act, 0xF,
        {p_lin("q_kal", 0.02, 0.005, 0.05), p_lin("res_atr", 2.2, 1.3, 3.2), p_lin("sl_atr", 1.2, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5), p_int("max_hold", 60, 25, 120)});
    add_f11<F11_AC05_DFA_RUNNER_NQ>(out, "engine_cpp_alpha_champion_05_5_multifractal_dfa_runner_nq", "Alpha Champion 05: Multifractal DFA Runner NQ", act, 0xF,
        {p_int("dfa_win", 30, 15, 50), p_lin("dfa_atr", 2.5, 1.5, 3.8), p_lin("sl_atr", 1.1, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC06_ZSCORE_ASYM_NQ>(out, "engine_cpp_alpha_champion_06_6_adaptive_zscore_asymtrail_nq", "Alpha Champion 06: Adaptive Z-Score MAD Asym Trail NQ", act, 0xF,
        {p_int("mad_win", 30, 15, 60), p_lin("z_thresh", 2.8, 1.8, 3.8), p_lin("sl_atr", 1.0, 0.6, 1.8), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC07_VOL_VACUUM_LVN_NQ>(out, "engine_cpp_alpha_champion_07_7_volume_vacuum_lvn_squeeze_nq", "Alpha Champion 07: Volume Vacuum LVN Squeeze NQ", act, 0xF,
        {p_lin("vel_atr", 1.8, 1.0, 3.0), p_lin("vol_mult", 2.0, 1.3, 3.5), p_lin("sl_atr", 1.0, 0.6, 1.8), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC08_MTF_5M_1M_FRACTAL_NQ>(out, "engine_cpp_alpha_champion_08_8_mtf_5m_1m_fractal_breakout_nq", "Alpha Champion 08: MTF 5m-1m Fractal Breakout NQ", act, 0xF,
        {p_int("lookback", 15, 8, 30), p_lin("prox_atr", 0.2, 0.05, 0.6), p_lin("sl_atr", 1.1, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC09_TOKYO_NY_DUAL_NQ>(out, "engine_cpp_alpha_champion_09_9_tokyo_ny_dual_impulse_nq", "Alpha Champion 09: Tokyo & NY Dual Impulse NQ", act, 0x5,
        {p_int("lookback", 10, 5, 25), p_lin("move_atr", 1.8, 1.0, 3.0), p_lin("sl_atr", 1.2, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC10_SETTLE_MOC_ASYM_NQ>(out, "engine_cpp_alpha_champion_10_10_settlement_moc_asym_target_nq", "Alpha Champion 10: Settlement MOC Asym Target NQ", act, 0x1,
        {p_int("lookback", 15, 8, 30), p_lin("ret_atr", 2.0, 1.2, 3.2), p_lin("sl_atr", 1.2, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC11_FISHER_CHAND_ES>(out, "engine_cpp_alpha_champion_11_11_fisher_chandelier_es", "Alpha Champion 11: Fisher Chandelier ES", act, 0xF, f11_chand_fisher_params(4.5e5, 1.2e-4, 1.2));
    add_f11<F11_AC12_KALMAN_CHAND_ES>(out, "engine_cpp_alpha_champion_12_12_kalman_chandelier_es", "Alpha Champion 12: Kalman Chandelier ES", act, 0xF,
        {p_lin("q_kal", 0.02, 0.005, 0.05), p_lin("res_atr", 2.2, 1.3, 3.2), p_lin("sl_atr", 1.2, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5), p_int("max_hold", 60, 25, 120)});
    add_f11<F11_AC13_ZSCORE_ASYM_ES>(out, "engine_cpp_alpha_champion_13_13_adaptive_zscore_es", "Alpha Champion 13: Adaptive Z-Score ES", act, 0xF,
        {p_int("mad_win", 30, 15, 60), p_lin("z_thresh", 2.8, 1.8, 3.8), p_lin("sl_atr", 1.0, 0.6, 1.8), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});
    add_f11<F11_AC14_FISHER_CHAND_YM>(out, "engine_cpp_alpha_champion_14_14_fisher_chandelier_ym", "Alpha Champion 14: Fisher Chandelier YM", act, 0xF, f11_chand_fisher_params(4.5e5, 1.2e-4, 1.2));
    add_f11<F11_AC15_KALMAN_CHAND_YM>(out, "engine_cpp_alpha_champion_15_15_kalman_chandelier_ym", "Alpha Champion 15: Kalman Chandelier YM", act, 0xF,
        {p_lin("q_kal", 0.02, 0.005, 0.05), p_lin("res_atr", 2.2, 1.3, 3.2), p_lin("sl_atr", 1.2, 0.7, 2.0), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5), p_int("max_hold", 60, 25, 120)});
    add_f11<F11_AC16_ZSCORE_ASYM_YM>(out, "engine_cpp_alpha_champion_16_16_adaptive_zscore_ym", "Alpha Champion 16: Adaptive Z-Score YM", act, 0xF,
        {p_int("mad_win", 30, 15, 60), p_lin("z_thresh", 2.8, 1.8, 3.8), p_lin("sl_atr", 1.0, 0.6, 1.8), p_lin("chand_trig", 2.0, 1.0, 3.5), p_lin("chand_trail", 1.5, 0.8, 2.5)});

    const std::string tqe = "ten_quant_strats_engine.cpp / master_100_multiverse_sweep.cpp";
    add_f11<F11_TQ0_KER_REGIME>(out, "ten_strats_workspace_0_ker_regime_switcher", "Ten Quant 0: KER Regime Switcher", tqe, 0xF,
        {p_int("er_lb", 30, 15, 50), p_lin("er_trend", 0.55, 0.40, 0.75), p_lin("er_chop", 0.25, 0.10, 0.40), p_lin("sl_atr", 1.6, 0.8, 2.5), p_lin("tp_atr", 3.2, 1.8, 4.5)});
    add_f11<F11_TQ1_LEADLAG_PULLBACK>(out, "ten_strats_workspace_1_leadlag_crossasset", "Ten Quant 1: Momentum Lead-Lag / Pullback", tqe, 0xC,
        {p_int("trend_lb", 60, 20, 64), p_int("fast_lb", 20, 5, 30), p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("tp_rr", 2.5, 1.5, 4.0), p_bool("rsi_mode", false)});
    add_f11<F11_TQ2_LDNNY_OPEN_RESET>(out, "ten_strats_workspace_2_ldnny_open_reset", "Ten Quant 2: London -> NY Open Reset", tqe, 0x4,
        {p_lin("probe_atr", 0.10, 0.02, 0.35), p_lin("sl_pad_atr", 0.25, 0.10, 0.60)});
    add_f11<F11_TQ3_MOC_INST_DRIFT>(out, "ten_strats_workspace_3_moc_institutional_drift", "Ten Quant 3: MOC Institutional Drift (15:15-15:55 ET)", tqe, 0x8,
        {p_lin("drift_atr", 1.0, 0.5, 2.0), p_lin("sl_atr", 1.8, 1.0, 3.0)});
    add_f11<F11_TQ4_AMT_VALUE_AREA>(out, "ten_strats_workspace_4_amt_value_area", "Ten Quant 4: AMT Value Area (London VA Reversion)", tqe, 0xC,
        {p_lin("probe_atr", 0.15, 0.05, 0.40), p_lin("sl_pad_atr", 0.25, 0.10, 0.60), p_bool("tp_opp_va", false)});
    add_f11<F11_TQ5_TURTLE_SOUP_ORB>(out, "ten_strats_workspace_5_turtle_soup_orb_fade", "Ten Quant 5: Turtle Soup ORB Fade", tqe, 0xC,
        {p_int("or_mins", 15, 15, 30, 2), p_lin("probe_min_atr", 0.15, 0.05, 0.40), p_lin("probe_max_atr", 1.2, 0.6, 2.0), p_lin("sl_pad_atr", 0.20, 0.10, 0.50)});
    add_f11<F11_TQ6_VSA_ABSORPTION>(out, "ten_strats_workspace_6_vsa_absorption", "Ten Quant 6: VSA Climatic Volume Absorption", tqe, 0xF,
        {p_lin("rvol_th", 2.0, 1.4, 3.2), p_lin("wick_frac", 0.50, 0.35, 0.70), p_lin("sl_pad_atr", 0.20, 0.10, 0.50), p_lin("rr", 2.5, 1.5, 4.0)});
    add_f11<F11_TQ7_BB_KC_SQUEEZE>(out, "ten_strats_workspace_7_bb_kc_volatility_squeeze", "Ten Quant 7: BB/KC Volatility Squeeze", tqe, 0xF,
        {p_int("period", 20, 10, 35), p_lin("bb_mult", 2.0, 1.5, 2.5), p_lin("kc_mult", 1.5, 1.0, 2.2), p_int("min_sq_bars", 5, 2, 12), p_lin("rr", 2.5, 1.5, 4.0)});
    add_f11<F11_TQ8_MULTIANCHOR_VWAP_MR>(out, "ten_strats_workspace_8_multianchor_vwap_mr", "Ten Quant 8: Multi-Anchor VWAP Midday MR", tqe, 0xC,
        {p_lin("z_entry", 2.1, 1.5, 3.0), p_lin("sl_sd", 1.2, 0.7, 2.2)});
    add_f11<F11_TQ9_DALTON_IB_EXT>(out, "ten_strats_workspace_9_dalton_ib_extension", "Ten Quant 9: Dalton IB Extension / Chop Classifier", tqe, 0xC,
        {p_lin("compress_atr", 1.6, 1.0, 2.2), p_lin("wide_atr", 2.5, 2.0, 3.5), p_lin("tp_ib_mult", 1.2, 0.8, 2.0), p_lin("sl_pad_atr", 0.4, 0.15, 0.80)});
}


// ============================================================================
// FAMILY 14: 14_Standalone_Workspaces_NQ_FVG_IV_Kronos (24 UIDs)
// Sources: fractal_iv_reversal, insta_batch2 (10), omar_no_wick, orb_nq (7),
//          live_bots (inverse_orb, ym_orb_retuned), laptop_reentry, kronos
// ============================================================================

enum F14Kind {
    F14_FRACTAL_CLUSTER = 0,
    F14_IV_WALL,
    F14_INSTA_BOS_FVG,
    F14_INSTA_EMA_VWAP,
    F14_INSTA_LIQ_BOS_VWAP,
    F14_INSTA_MTF_FVG,
    F14_INSTA_OPENING_RANGE,
    F14_INSTA_PO3_IFVG,
    F14_INSTA_PO3_SMT,
    F14_INSTA_PREM_DISC,
    F14_INSTA_SESSION_2CANDLE,
    F14_INSTA_SESSION_LIQ,
    F14_OMAR_NO_WICK_5M,
    F14_ORB_NQ_ORB,
    F14_ORB_NQ_1H_5M_DOLLAR,
    F14_ORB_NQ_CLEAN,
    F14_ORB_NQ_CLEAN_CLOSE_STOP,
    F14_ORB_NQ_CLEAN_DOLLAR,
    F14_ORB_NQ_HYBRID_STOP,
    F14_ORB_NQ_PROP_RISK,
    F14_PROJ_INVERSE_ORB,
    F14_PROJ_YM_ORB_TRAIL_STEP,
    F14_LAPTOP_REENTRY_SUITE,
    F14_KRONOS_SIGNAL
};

struct F14State {
    F14Kind kind = F14_FRACTAL_CLUSTER;
    u11::InstrumentSpec inst{};
    double p0 = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0, p4 = 0.0, p5 = 0.0;
    int max_hold = 90;
    int max_trades_day = 4;

    int last_day = -1;
    int trades_today = 0;
    int last_sess = -99;
    int sess_bars = 0;

    // Session 2-candle range (insta_batch2_session_2candle)
    double c2_h = -1e18, c2_l = 1e18;

    // Multi-TF FVG & BOS/FVG & Session Liquidity state
    double fvg_top = 0.0, fvg_bot = 0.0;
    int8_t fvg_dir = 0;
    int fvg_age = 999;
    double htf_top = 0.0, htf_bot = 0.0;
    int8_t htf_dir = 0;
    int htf_age = 999;
    bool swept_hi = false, swept_lo = false;

    // Causal 5m OHLCV builder (Omar Nowick 5m, ORB 5m)
    int cur_5m_bucket = -1;
    double b5_o = 0.0, b5_h = 0.0, b5_l = 0.0, b5_c = 0.0;
    struct Bar5m { double o, h, l, c; };
    RingBuf<Bar5m, 20> bars5m;

    // Omar external structure state
    double conf_hh = -1e18, conf_ll = 1e18, conf_hl = 1e18, conf_lh = -1e18;
    double low_since_hh = 1e18, high_since_ll = -1e18;
    int trend_state = 0, prev_trend = 0;

    // Causal 4H OHLC builder (PO3 IFVG)
    int cur_4h_bucket = -1;
    double h4_o = 0.0, h4_h = 0.0, h4_l = 0.0, h4_c = 0.0;
    int h4_bias = 0;

    // Re-entry counter (laptop_reentry)
    int entry_num_today = 0;
    bool prev_in_pos = false;

    // Kronos tokenized candlestick ring buffer (256 causal bars)
    struct TokBar { uint16_t tok; double next_ret_atr; };
    RingBuf<TokBar, 256> kronos_hist;
    uint16_t prev_tok = 0;
    double prev_c_k = 0.0;
    bool has_prev_k = false;
};

template <F14Kind K>
inline void f14_init(F14State& st, const std::vector<double>& params, const u11::InstrumentSpec* spec) {
    st = F14State{};
    st.kind = K;
    if (spec) st.inst = *spec;
    switch (K) {
        case F14_FRACTAL_CLUSTER:
            st.p0 = p_at(params, 0, 50.0);  // window
            st.p1 = p_at(params, 1, 2.0);   // z_thresh
            st.p2 = p_at(params, 2, 1.0);   // swing_score_min
            st.p3 = p_at(params, 3, 1.5);   // sl_atr
            st.p4 = p_at(params, 4, 2.0);   // rr
            st.max_trades_day = 6;
            break;
        case F14_IV_WALL:
            st.p0 = p_at(params, 0, 0.0);   // estimator (0=ATR,1=Parkinson,2=GK,3=YZ,4=EWMA,5=GapAdj)
            st.p1 = p_at(params, 1, 20.0);  // vol_period
            st.p2 = p_at(params, 2, 1.0);   // wall mult
            st.p3 = p_at(params, 3, 1.5);   // sl_atr
            st.p4 = p_at(params, 4, 2.0);   // rr
            st.max_trades_day = 6;
            break;
        case F14_INSTA_BOS_FVG:
            st.p0 = p_at(params, 0, 10.0);  // swing_window
            st.p1 = p_at(params, 1, 15.0);  // fvg_lookback
            st.p2 = p_at(params, 2, 2.0);   // rr
            st.p3 = p_at(params, 3, 0.1);   // min_fvg_atr
            st.p4 = p_at(params, 4, 1.0);   // sl_atr_mult
            st.max_trades_day = 5;
            break;
        case F14_INSTA_EMA_VWAP:
            st.p0 = p_at(params, 0, 1.5);   // atr_stop_mult
            st.p1 = p_at(params, 1, 3.0);   // atr_target_mult
            st.max_trades_day = 6;
            break;
        case F14_INSTA_LIQ_BOS_VWAP:
            st.p0 = p_at(params, 0, 20.0);  // liq_lb
            st.p1 = p_at(params, 1, 3.0);   // mom_lb
            st.p2 = p_at(params, 2, 0.5);   // risk_mult (<1 scalp)
            st.p3 = p_at(params, 3, 1.2);   // sl_atr
            st.max_trades_day = 6;
            break;
        case F14_INSTA_MTF_FVG:
            st.p0 = p_at(params, 0, 5.0);   // htf bars (5m)
            st.p1 = p_at(params, 1, 30.0);  // max_htf_age
            st.p2 = p_at(params, 2, 5.0);   // max_ltf_age
            st.p3 = p_at(params, 3, 1.0);   // rr (1:1)
            st.p4 = p_at(params, 4, 0.2);   // sl_pad_atr
            st.max_trades_day = 5;
            break;
        case F14_INSTA_OPENING_RANGE:
            st.p0 = p_at(params, 0, 15.0);  // or_mins
            st.p1 = p_at(params, 1, 0.0);   // buffer_pts
            st.p2 = p_at(params, 2, 1.0);   // rr
            st.p3 = p_at(params, 3, 0.0);   // use_prior_close
            st.max_trades_day = 1;
            st.max_hold = 240;
            break;
        case F14_INSTA_PO3_IFVG:
            st.p0 = p_at(params, 0, 0.35);  // min_wick_pct
            st.p1 = p_at(params, 1, 25.0);  // ifvg_lb
            st.p2 = p_at(params, 2, 18.0);  // swing_lb
            st.p3 = p_at(params, 3, 3.0);   // rr
            st.max_trades_day = 3;
            break;
        case F14_INSTA_PO3_SMT:
            st.p0 = p_at(params, 0, 30.0);  // range lookback
            st.p1 = p_at(params, 1, 0.5);   // sl_atr
            st.p2 = p_at(params, 2, 2.0);   // target_r
            st.p3 = p_at(params, 3, 1.0);   // require_smt_div (>0.5 when ref avail)
            st.max_trades_day = 4;
            break;
        case F14_INSTA_PREM_DISC:
            st.p0 = p_at(params, 0, 40.0);  // lookback
            st.p1 = p_at(params, 1, 0.25);  // discount_th
            st.p2 = p_at(params, 2, 0.75);  // premium_th
            st.p3 = p_at(params, 3, 0.10);  // stop_offset_frac
            st.max_trades_day = 6;
            break;
        case F14_INSTA_SESSION_2CANDLE:
            st.p0 = p_at(params, 0, 4.0);   // min_range pts
            st.p1 = p_at(params, 1, 1.5);   // rr
            st.max_trades_day = 3;
            break;
        case F14_INSTA_SESSION_LIQ:
            st.p0 = p_at(params, 0, 60.0);  // lookback
            st.p1 = p_at(params, 1, 0.3);   // disp_atr
            st.p2 = p_at(params, 2, 2.0);   // rr
            st.p3 = p_at(params, 3, 0.2);   // sl_pad_atr
            st.max_trades_day = 3;
            break;
        case F14_OMAR_NO_WICK_5M:
            st.p0 = p_at(params, 0, 5.0);   // min_sl_pts
            st.p1 = p_at(params, 1, 1.10);  // sl_breath (10%)
            st.p2 = p_at(params, 2, 1.0);   // rr (1:1)
            st.max_trades_day = 3;
            st.max_hold = 120;
            break;
        case F14_ORB_NQ_ORB:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 2.0);   // rr
            st.p2 = p_at(params, 2, 1.0);   // stop_mult
            st.max_trades_day = 1;
            st.max_hold = 390;
            break;
        case F14_ORB_NQ_1H_5M_DOLLAR:
            st.p0 = p_at(params, 0, 60.0);  // 1h OR
            st.p1 = p_at(params, 1, 400.0); // dollar_stop ($ per 1 mini unit baseline)
            st.p2 = p_at(params, 2, 800.0); // dollar_target
            st.max_trades_day = 1;
            st.max_hold = 360;
            break;
        case F14_ORB_NQ_CLEAN:
        case F14_ORB_NQ_CLEAN_CLOSE_STOP:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, (K == F14_ORB_NQ_CLEAN) ? 1.0 : 0.5);
            st.max_trades_day = 1;
            st.max_hold = 390;
            break;
        case F14_ORB_NQ_CLEAN_DOLLAR:
        case F14_ORB_NQ_PROP_RISK:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 0.5);   // stop_mult
            st.p2 = p_at(params, 2, 500.0); // dollar_stop
            st.p3 = p_at(params, 3, 1000.0);// dollar_target
            st.max_trades_day = 1;
            st.max_hold = 390;
            break;
        case F14_ORB_NQ_HYBRID_STOP:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 0.5);   // soft_mult
            st.p2 = p_at(params, 2, 1.0);   // hard_mult
            st.max_trades_day = 1;
            st.max_hold = 390;
            break;
        case F14_PROJ_INVERSE_ORB:
            st.p0 = p_at(params, 0, 15.0);
            st.p1 = p_at(params, 1, 1.0);   // sl_or_mult
            st.p2 = p_at(params, 2, 2.0);   // rr
            st.max_trades_day = 4;
            st.max_hold = 240;
            break;
        case F14_PROJ_YM_ORB_TRAIL_STEP:
            st.p0 = p_at(params, 0, 15.0);  // or_mins (swept champion 15m)
            st.p1 = p_at(params, 1, 0.05);  // trig_frac
            st.p2 = p_at(params, 2, 1.79);  // sint_atr/or mult (trail stop)
            st.p3 = p_at(params, 3, 0.90);  // lock
            st.p4 = p_at(params, 4, 5.0);   // rr
            st.max_trades_day = 4;
            st.max_hold = 360;
            break;
        case F14_LAPTOP_REENTRY_SUITE:
            st.p0 = p_at(params, 0, 15.0);  // or_mins
            st.p1 = p_at(params, 1, 1.0);   // fade entry 1 (>0.5)
            st.p2 = p_at(params, 2, 1.0);   // sl_or_mult
            st.p3 = p_at(params, 3, 2.0);   // rr
            st.max_trades_day = static_cast<int>(std::round(p_at(params, 4, 4.0)));
            st.max_hold = 180;
            break;
        case F14_KRONOS_SIGNAL:
            st.p0 = p_at(params, 0, 3.0);   // pattern len
            st.p1 = p_at(params, 1, 0.35);  // min expected ret (atr)
            st.p2 = p_at(params, 2, 1.2);   // sl_atr
            st.p3 = p_at(params, 3, 2.4);   // tp_atr
            st.max_trades_day = 6;
            st.max_hold = 45;
            break;
    }
}

inline uint16_t f14_tokenize_bar(double o, double h, double l, double c, double atr) {
    double a = std::max(atr, 1e-4);
    double rng = std::max(1e-4, h - l);
    double body = c - o;
    int dir = (body >= 0.0) ? 1 : 0;
    int bmag = std::clamp(static_cast<int>(std::abs(body) / (0.4 * a)), 0, 3);
    int rmag = std::clamp(static_cast<int>(rng / (0.6 * a)), 0, 3);
    double uw = (h - std::max(o, c)) / rng;
    double lw = (std::min(o, c) - l) / rng;
    int wb = ((uw > 0.35) ? 2 : 0) | ((lw > 0.35) ? 1 : 0);
    return static_cast<uint16_t>((dir << 6) | (bmag << 4) | (rmag << 2) | wb);
}

inline u11::PluginSignal f14_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto& st = *static_cast<F14State*>(state_ptr);
    u11::PluginSignal sig{};

    if (view.day_yyyymmdd() != st.last_day) {
        st.last_day = view.day_yyyymmdd();
        st.trades_today = 0;
        st.entry_num_today = 0;
        st.swept_hi = false;
        st.swept_lo = false;
    }
    int sess = view.session_id();
    if (sess != st.last_sess) {
        st.last_sess = sess;
        st.sess_bars = 0;
        st.c2_h = -1e18; st.c2_l = 1e18;
    }
    st.sess_bars++;

    if (!st.prev_in_pos && view.in_position) st.entry_num_today++;
    st.prev_in_pos = view.in_position;

    const auto& b = view[0];
    double o = static_cast<double>(b.o), h = static_cast<double>(b.h);
    double l = static_cast<double>(b.l), c = static_cast<double>(b.c);
    if (st.sess_bars <= 2) {
        st.c2_h = std::max(st.c2_h, h);
        st.c2_l = std::min(st.c2_l, l);
    }

    int em = view.et_mins();
    double cur_atr = std::max(view.atr14(0), 2.0 * st.inst.tick_size);

    // Causal 5m aggregator & Omar market structure tracker
    int bucket5 = view.day_yyyymmdd() * 300 + (em / 5);
    if (bucket5 != st.cur_5m_bucket) {
        st.cur_5m_bucket = bucket5;
        st.b5_o = o; st.b5_h = h; st.b5_l = l; st.b5_c = c;
    } else {
        st.b5_h = std::max(st.b5_h, h);
        st.b5_l = std::min(st.b5_l, l);
        st.b5_c = c;
    }
    bool completed_5m = (em % 5 == 4);
    if (completed_5m && st.kind == F14_OMAR_NO_WICK_5M) {
        st.bars5m.push({st.b5_o, st.b5_h, st.b5_l, st.b5_c});
        st.prev_trend = st.trend_state;
        int n5 = st.bars5m.size();
        if (n5 >= 3) {
            const auto& m0 = st.bars5m.back(0);
            const auto& m1 = st.bars5m.back(1);
            const auto& m2 = st.bars5m.back(2);
            if (st.conf_hh > -1e17 && st.conf_ll < 1e17) {
                st.low_since_hh = std::min(st.low_since_hh, m0.l);
                st.high_since_ll = std::max(st.high_since_ll, m0.h);
                if (m0.c > st.conf_hh) {
                    st.conf_hl = st.low_since_hh;
                    st.conf_hh = m0.c;
                    st.trend_state = 1;
                    st.low_since_hh = m0.l;
                } else if (m0.c < st.conf_ll) {
                    st.conf_lh = st.high_since_ll;
                    st.conf_ll = m0.c;
                    st.trend_state = -1;
                    st.high_since_ll = m0.h;
                }
            } else {
                if (m1.h > m2.h && m1.h > m0.h) { st.conf_hh = m1.h; st.low_since_hh = m0.l; }
                if (m1.l < m2.l && m1.l < m0.l) { st.conf_ll = m1.l; st.high_since_ll = m0.h; }
                if (st.conf_hh > st.conf_ll) {
                    st.trend_state = (m0.c >= st.conf_ll) ? 1 : -1;
                    st.conf_hl = st.conf_ll; st.conf_lh = st.conf_hh;
                }
            }
        }
    }

    // Causal 4H candle bias tracker (PO3 IFVG)
    if (st.kind == F14_INSTA_PO3_IFVG) {
        int b4h = view.day_yyyymmdd() * 6 + (em / 240);
        if (b4h != st.cur_4h_bucket) {
            if (st.cur_4h_bucket >= 0) {
                double rng4 = std::max(1e-4, st.h4_h - st.h4_l);
                double body4 = st.h4_c - st.h4_o;
                double lw = (std::min(st.h4_o, st.h4_c) - st.h4_l) / rng4;
                double uw = (st.h4_h - std::max(st.h4_o, st.h4_c)) / rng4;
                if (body4 > 0.0 && lw >= st.p0) st.h4_bias = 1;
                else if (body4 < 0.0 && uw >= st.p0) st.h4_bias = -1;
                else st.h4_bias = 0;
            }
            st.cur_4h_bucket = b4h;
            st.h4_o = o; st.h4_h = h; st.h4_l = l; st.h4_c = c;
        } else {
            st.h4_h = std::max(st.h4_h, h);
            st.h4_l = std::min(st.h4_l, l);
            st.h4_c = c;
        }
    }

    // Causal Kronos tokenized history update
    if (st.kind == F14_KRONOS_SIGNAL) {
        uint16_t cur_tok = f14_tokenize_bar(o, h, l, c, cur_atr);
        if (st.has_prev_k) {
            double ret_a = (c - st.prev_c_k) / cur_atr;
            st.kronos_hist.push({st.prev_tok, ret_a});
        }
        st.prev_tok = cur_tok;
        st.prev_c_k = c;
        st.has_prev_k = true;
    }

    if (view.bar_index() < 65) return sig;
    double sc = rel_scale(view, st.inst);
    double pt_s = sym_pt_scale(st.inst);
    double p_c = static_cast<double>(view[-1].c);
    double p_h = static_cast<double>(view[-1].h), p_l = static_cast<double>(view[-1].l);

    if (view.in_position || st.trades_today >= st.max_trades_day) return sig;

    switch (st.kind) {
        case F14_FRACTAL_CLUSTER: {
            int win = std::clamp(static_cast<int>(std::round(st.p0)), 20, 64);
            double sum_c = 0.0, sum_sq = 0.0;
            double arr[64];
            for (int k = 1; k <= win; ++k) {
                double ck = static_cast<double>(view[-k].c);
                arr[k - 1] = ck;
                sum_c += ck;
            }
            double mu = sum_c / static_cast<double>(win);
            for (int k = 0; k < win; ++k) { double d = arr[k] - mu; sum_sq += d * d; }
            double sd = std::sqrt(sum_sq / static_cast<double>(win));
            if (sd <= 1e-4) return sig;
            std::sort(arr, arr + win);
            double p10 = arr[std::max(0, win / 10)];
            double p90 = arr[std::min(win - 1, (win * 9) / 10)];
            double z = (c - mu) / sd;
            int sh_score = 0, sl_score = 0;
            const int lbs[3] = {5, 10, 20};
            for (int j = 0; j < 3; ++j) {
                double rh = -1e18, rl = 1e18;
                for (int k = 1; k <= lbs[j]; ++k) {
                    rh = std::max(rh, static_cast<double>(view[-k].h));
                    rl = std::min(rl, static_cast<double>(view[-k].l));
                }
                if (h > rh) sh_score++;
                if (l < rl) sl_score++;
            }
            int min_sc = static_cast<int>(std::round(st.p2));
            double sl = st.p3 * cur_atr;
            if (c <= p10 && z <= -st.p1 && sl_score >= min_sc) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            if (c >= p90 && z >= st.p1 && sh_score >= min_sc) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            break;
        }
        case F14_IV_WALL: {
            int vtype = static_cast<int>(std::round(st.p0)) % 6;
            int per = std::clamp(static_cast<int>(std::round(st.p1)), 5, 60);
            double exp_rng = cur_atr;
            if (vtype == 1) { // Parkinson
                double s = 0.0;
                for (int k = 1; k <= per; ++k) {
                    double r = std::log(std::max(1.0, static_cast<double>(view[-k].h)) / std::max(1.0, static_cast<double>(view[-k].l)));
                    s += r * r;
                }
                double sigma = std::sqrt(std::max(0.0, (s / static_cast<double>(per)) / (4.0 * std::log(2.0))));
                exp_rng = p_c * sigma;
            } else if (vtype == 2) { // Garman-Klass
                double s = 0.0;
                for (int k = 1; k <= per; ++k) {
                    double hl = std::log(std::max(1.0, static_cast<double>(view[-k].h)) / std::max(1.0, static_cast<double>(view[-k].l)));
                    double co = std::log(std::max(1.0, static_cast<double>(view[-k].c)) / std::max(1.0, static_cast<double>(view[-k].o)));
                    s += 0.5 * hl * hl - (2.0 * std::log(2.0) - 1.0) * co * co;
                }
                double sigma = std::sqrt(std::max(0.0, s / static_cast<double>(per)));
                exp_rng = p_c * sigma;
            } else if (vtype == 3 || vtype == 4) { // Yang-Zhang / EWMA approx
                double s = 0.0;
                for (int k = 1; k <= per; ++k) {
                    double r = std::log(std::max(1.0, static_cast<double>(view[-k].c)) / std::max(1.0, static_cast<double>(view[-k - 1].c)));
                    s += r * r;
                }
                exp_rng = p_c * std::sqrt(std::max(0.0, s / static_cast<double>(per)));
            } else if (vtype == 5) { // Gap-adjusted ATR
                double gap = 0.0;
                for (int k = 1; k <= per; ++k) {
                    gap += std::abs(static_cast<double>(view[-k].o) - static_cast<double>(view[-k - 1].c));
                }
                exp_rng = cur_atr + (gap / static_cast<double>(per));
            }
            exp_rng = std::max(cur_atr * 0.5, exp_rng * st.p2);
            double upper = p_c + exp_rng, lower = p_c - exp_rng;
            double sl = st.p3 * cur_atr;
            if (l <= lower && c > lower) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            if (h >= upper && c < upper) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p4, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            break;
        }
        case F14_INSTA_BOS_FVG: {
            int sw = std::clamp(static_cast<int>(std::round(st.p0)), 5, 40);
            int max_age = std::clamp(static_cast<int>(std::round(st.p1)), 5, 40);
            double sh = -1e18, sl_w = 1e18;
            for (int k = 2; k <= sw + 1; ++k) {
                sh = std::max(sh, static_cast<double>(view[-k].h));
                sl_w = std::min(sl_w, static_cast<double>(view[-k].l));
            }
            double min_g = st.p3 * cur_atr;
            // Preceding 3-bar FVG (-3..-1)
            double b3h = static_cast<double>(view[-3].h), b3l = static_cast<double>(view[-3].l);
            if (p_c > sh && p_l > b3h + min_g) {
                st.fvg_top = p_l; st.fvg_bot = b3h; st.fvg_dir = 1; st.fvg_age = 0;
            } else if (p_c < sl_w && b3l > p_h + min_g) {
                st.fvg_top = b3l; st.fvg_bot = p_h; st.fvg_dir = -1; st.fvg_age = 0;
            } else if (st.fvg_dir != 0) {
                st.fvg_age++;
            }
            if (st.fvg_dir != 0 && st.fvg_age > 0 && st.fvg_age <= max_age) {
                double sl = st.p4 * cur_atr;
                if (st.fvg_dir == 1 && l <= st.fvg_top && h >= st.fvg_bot && c > o) {
                    st.fvg_dir = 0; st.trades_today++;
                    return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                }
                if (st.fvg_dir == -1 && h >= st.fvg_bot && l <= st.fvg_top && c < o) {
                    st.fvg_dir = 0; st.trades_today++;
                    return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 3, st.inst);
                }
            }
            break;
        }
        case F14_INSTA_EMA_VWAP: {
            double pv = view.session_vwap(-1);
            double e9 = view.ema9(-1), e20 = view.ema20(-1);
            double e9_2 = view.ema9(-2), e20_2 = view.ema20(-2);
            double sl = st.p0 * cur_atr, tp = st.p1 * cur_atr;
            if (p_c > pv && e9 > e20 && e9_2 <= e20_2) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            if (p_c < pv && e9 < e20 && e9_2 >= e20_2) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            break;
        }
        case F14_INSTA_LIQ_BOS_VWAP: {
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 10, 40);
            int mlb = std::clamp(static_cast<int>(std::round(st.p1)), 2, 10);
            double rh = -1e18, rl = 1e18;
            for (int k = 2; k <= lb + 1; ++k) {
                rh = std::max(rh, static_cast<double>(view[-k].h));
                rl = std::min(rl, static_cast<double>(view[-k].l));
            }
            double pv = view.session_vwap(-1);
            double mom = p_c - static_cast<double>(view[-1 - mlb].c);
            double sl = st.p3 * cur_atr;
            double tp = sl * std::clamp(st.p2, 0.25, 0.95);
            if (p_c > rh && p_c > pv && mom > 0.0) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            if (p_c < rl && p_c < pv && mom < 0.0) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            break;
        }
        case F14_INSTA_MTF_FVG: {
            int htf_step = std::clamp(static_cast<int>(std::round(st.p0)), 3, 15);
            int max_h = std::clamp(static_cast<int>(std::round(st.p1)), 10, 60);
            int max_l = std::clamp(static_cast<int>(std::round(st.p2)), 2, 15);
            // Causal HTF FVG check at step boundaries
            if (view.bar_index() % static_cast<size_t>(htf_step) == 0 && view.bar_index() >= static_cast<size_t>(3 * htf_step + 5)) {
                double h0l = 1e18, h0h = -1e18, h2l = 1e18, h2h = -1e18;
                for (int k = 1; k <= htf_step; ++k) {
                    h0h = std::max(h0h, static_cast<double>(view[-k].h));
                    h0l = std::min(h0l, static_cast<double>(view[-k].l));
                    h2h = std::max(h2h, static_cast<double>(view[-2 * htf_step - k].h));
                    h2l = std::min(h2l, static_cast<double>(view[-2 * htf_step - k].l));
                }
                if (h0l > h2h + 0.05 * cur_atr) { st.htf_top = h0l; st.htf_bot = h2h; st.htf_dir = 1; st.htf_age = 0; }
                else if (h2l > h0h + 0.05 * cur_atr) { st.htf_top = h2l; st.htf_bot = h0h; st.htf_dir = -1; st.htf_age = 0; }
            }
            st.htf_age++;
            // LTF FVG inside HTF zone
            double b3h = static_cast<double>(view[-3].h), b3l = static_cast<double>(view[-3].l);
            if (st.htf_dir == 1 && st.htf_age <= max_h && p_l <= st.htf_top && b3l > p_h) {
                st.fvg_top = b3l; st.fvg_bot = p_h; st.fvg_dir = -1; st.fvg_age = 0;
            } else if (st.htf_dir == -1 && st.htf_age <= max_h && p_h >= st.htf_bot && p_l > b3h) {
                st.fvg_top = p_l; st.fvg_bot = b3h; st.fvg_dir = 1; st.fvg_age = 0;
            } else {
                st.fvg_age++;
            }
            if (st.htf_dir == 1 && st.fvg_dir == -1 && st.fvg_age <= max_l && c > st.fvg_top) {
                double sl = std::max(cur_atr, (c - st.fvg_bot) + st.p4 * cur_atr);
                st.fvg_dir = 0; st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            if (st.htf_dir == -1 && st.fvg_dir == 1 && st.fvg_age <= max_l && c < st.fvg_bot) {
                double sl = std::max(cur_atr, (st.fvg_top - c) + st.p4 * cur_atr);
                st.fvg_dir = 0; st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            break;
        }
        case F14_INSTA_OPENING_RANGE: {
            int om = (st.p0 >= 45) ? 60 : (st.p0 >= 25 ? 30 : 15);
            if ((sess != 2 && sess != 3) || !view.or_formed(om)) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double rng = oh - ol;
            if (rng <= 2.0 * st.inst.tick_size) return sig;
            double buf = st.p1 * pt_s;
            bool prior_ok_l = (st.p3 < 0.5) || !view.has_prior_day() || (oh > view.pdc());
            bool prior_ok_s = (st.p3 < 0.5) || !view.has_prior_day() || (ol < view.pdc());
            if (h >= oh + buf && p_h < oh + buf && prior_ok_l) {
                double sl = std::max(cur_atr, c - ol);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 10, st.inst);
            }
            if (l <= ol - buf && p_l > ol - buf && prior_ok_s) {
                double sl = std::max(cur_atr, oh - c);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 10, st.inst);
            }
            break;
        }
        case F14_INSTA_PO3_IFVG: {
            int lb = std::clamp(static_cast<int>(std::round(st.p1)), 8, 40);
            int sw = std::clamp(static_cast<int>(std::round(st.p2)), 8, 40);
            if (st.h4_bias == 1) {
                for (int k = 1; k < lb; ++k) {
                    double hi = static_cast<double>(view[-k - 1].h), lo = static_cast<double>(view[-k].l);
                    if (hi < lo) { // downward gap inverted bullish
                        if (c > lo && p_c <= lo) {
                            double sl_lvl = 1e18;
                            for (int j = 1; j <= sw; ++j) sl_lvl = std::min(sl_lvl, static_cast<double>(view[-j].l));
                            double sl = std::max(cur_atr, c - sl_lvl);
                            st.trades_today++;
                            return make_sig(1, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 5, st.inst);
                        }
                        break;
                    }
                }
            } else if (st.h4_bias == -1) {
                for (int k = 1; k < lb; ++k) {
                    double lo = static_cast<double>(view[-k - 1].l), hi = static_cast<double>(view[-k].h);
                    if (lo > hi) {
                        if (c < hi && p_c >= hi) {
                            double sh_lvl = -1e18;
                            for (int j = 1; j <= sw; ++j) sh_lvl = std::max(sh_lvl, static_cast<double>(view[-j].h));
                            double sl = std::max(cur_atr, sh_lvl - c);
                            st.trades_today++;
                            return make_sig(-1, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 5, st.inst);
                        }
                        break;
                    }
                }
            }
            break;
        }
        case F14_INSTA_PO3_SMT: {
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 10, 60);
            double rh = -1e18, rl = 1e18;
            for (int k = 1; k <= lb; ++k) {
                rh = std::max(rh, static_cast<double>(view[-k].h));
                rl = std::min(rl, static_cast<double>(view[-k].l));
            }
            bool smt_bear = true, smt_bull = true;
            if (st.p3 > 0.5 && has_distinct_ref(view, st.inst)) {
                double ref_rh = -1e18, ref_rl = 1e18;
                for (int k = 1; k <= lb; ++k) {
                    const auto& rb = ref_bar(view, st.inst, -k);
                    ref_rh = std::max(ref_rh, static_cast<double>(rb.h));
                    ref_rl = std::min(ref_rl, static_cast<double>(rb.l));
                }
                const auto& rb0 = ref_bar(view, st.inst, 0);
                smt_bear = (static_cast<double>(rb0.h) <= ref_rh);
                smt_bull = (static_cast<double>(rb0.l) >= ref_rl);
            }
            double sl = st.p1 * cur_atr;
            if (h > rh && c < rh && smt_bear) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            if (l < rl && c > rl && smt_bull) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 4, st.inst);
            }
            break;
        }
        case F14_INSTA_PREM_DISC: {
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 15, 64);
            double rh = -1e18, rl = 1e18;
            for (int k = 1; k <= lb; ++k) {
                rh = std::max(rh, static_cast<double>(view[-k].h));
                rl = std::min(rl, static_cast<double>(view[-k].l));
            }
            double rw = rh - rl;
            if (rw <= 2.0 * cur_atr) return sig;
            double pos = std::clamp((c - rl) / rw, 0.0, 1.0);
            double prev_pos = std::clamp((p_c - rl) / rw, 0.0, 1.0);
            if (pos <= st.p1 && prev_pos > st.p1) {
                double sl = std::max(cur_atr, (c - rl) + st.p3 * rw);
                double tp = std::max(cur_atr, rh - c);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            if (pos >= st.p2 && prev_pos < st.p2) {
                double sl = std::max(cur_atr, (rh - c) + st.p3 * rw);
                double tp = std::max(cur_atr, c - rl);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 3, st.inst);
            }
            break;
        }
        case F14_INSTA_SESSION_2CANDLE: {
            if (st.sess_bars <= 2 || st.c2_h <= st.c2_l) return sig;
            double rw = st.c2_h - st.c2_l;
            if (rw < st.p0 * pt_s) return sig;
            if (p_c > st.c2_h && static_cast<double>(view[-2].c) <= st.c2_h) {
                double sl = std::max(cur_atr, c - st.c2_l);
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            if (p_c < st.c2_l && static_cast<double>(view[-2].c) >= st.c2_l) {
                double sl = std::max(cur_atr, st.c2_h - c);
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p1, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F14_INSTA_SESSION_LIQ: {
            if (sess != 2 && sess != 3) return sig;
            int lb = std::clamp(static_cast<int>(std::round(st.p0)), 20, 64);
            double liq_h = -1e18, liq_l = 1e18;
            for (int k = 2; k <= lb; ++k) {
                liq_h = std::max(liq_h, static_cast<double>(view[-k].h));
                liq_l = std::min(liq_l, static_cast<double>(view[-k].l));
            }
            if (p_h > liq_h) st.swept_hi = true;
            if (p_l < liq_l) st.swept_lo = true;
            double b3h = static_cast<double>(view[-3].h), b3l = static_cast<double>(view[-3].l);
            double body1 = std::abs(p_c - static_cast<double>(view[-1].o));
            if (body1 >= st.p1 * cur_atr) {
                if (p_l > b3h) { st.fvg_top = p_l; st.fvg_bot = b3h; st.fvg_dir = 1; st.fvg_age = 0; }
                else if (b3l > p_h) { st.fvg_top = b3l; st.fvg_bot = p_h; st.fvg_dir = -1; st.fvg_age = 0; }
            }
            if (st.swept_lo && st.fvg_dir == -1 && c > st.fvg_top) {
                double sl = std::max(cur_atr, (c - st.fvg_bot) + st.p3 * cur_atr);
                st.fvg_dir = 0; st.swept_lo = false; st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            if (st.swept_hi && st.fvg_dir == 1 && c < st.fvg_bot) {
                double sl = std::max(cur_atr, (st.fvg_top - c) + st.p3 * cur_atr);
                st.fvg_dir = 0; st.swept_hi = false; st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F14_OMAR_NO_WICK_5M: {
            if (!completed_5m || (sess != 2 && sess != 3)) return sig;
            double min_sl = st.p0 * pt_s;
            bool nowick_bull = (std::abs(st.b5_l - st.b5_o) <= 2.0 * st.inst.tick_size) && (st.b5_c >= st.b5_o);
            bool nowick_bear = (std::abs(st.b5_h - st.b5_o) <= 2.0 * st.inst.tick_size) && (st.b5_c <= st.b5_o);
            if (st.trend_state == 1 && (st.prev_trend != 1 || nowick_bull) && st.conf_hl < 1e17) {
                double raw_sl = (c - st.conf_hl) * st.p1;
                if (raw_sl >= min_sl) {
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, raw_sl, raw_sl * st.p2, 0, 0, 0, 0, st.max_hold, 5, st.inst);
                }
            }
            if (st.trend_state == -1 && (st.prev_trend != -1 || nowick_bear) && st.conf_lh > -1e17) {
                double raw_sl = (st.conf_lh - c) * st.p1;
                if (raw_sl >= min_sl) {
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, raw_sl, raw_sl * st.p2, 0, 0, 0, 0, st.max_hold, 5, st.inst);
                }
            }
            break;
        }
        case F14_ORB_NQ_ORB:
        case F14_ORB_NQ_1H_5M_DOLLAR:
        case F14_ORB_NQ_CLEAN:
        case F14_ORB_NQ_CLEAN_CLOSE_STOP:
        case F14_ORB_NQ_CLEAN_DOLLAR:
        case F14_ORB_NQ_HYBRID_STOP:
        case F14_ORB_NQ_PROP_RISK: {
            int om = (st.p0 >= 45) ? 60 : (st.p0 >= 25 ? 30 : 15);
            if ((sess != 2 && sess != 3) || !view.or_formed(om)) return sig;
            if (st.kind == F14_ORB_NQ_1H_5M_DOLLAR && !completed_5m) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double rng = oh - ol;
            if (rng <= 2.0 * st.inst.tick_size) return sig;
            int8_t dir = 0;
            if (h >= oh && p_h < oh) dir = 1;
            else if (l <= ol && p_l > ol) dir = -1;
            if (dir == 0) return sig;
            double sl = rng, tp = 0.0;
            if (st.kind == F14_ORB_NQ_ORB) {
                sl = rng * st.p2; tp = sl * st.p1;
            } else if (st.kind == F14_ORB_NQ_1H_5M_DOLLAR) {
                double d_sl = st.p1 / std::max(0.5, st.inst.mini_pv);
                double d_tp = st.p2 / std::max(0.5, st.inst.mini_pv);
                sl = std::min(0.5 * rng, d_sl); tp = d_tp;
            } else if (st.kind == F14_ORB_NQ_CLEAN || st.kind == F14_ORB_NQ_CLEAN_CLOSE_STOP) {
                sl = rng * st.p1; tp = 0.0;
            } else if (st.kind == F14_ORB_NQ_CLEAN_DOLLAR || st.kind == F14_ORB_NQ_PROP_RISK) {
                double d_sl = st.p2 / std::max(0.5, st.inst.mini_pv);
                double d_tp = st.p3 / std::max(0.5, st.inst.mini_pv);
                sl = std::min(rng * st.p1, d_sl); tp = d_tp;
            } else if (st.kind == F14_ORB_NQ_HYBRID_STOP) {
                sl = rng * st.p2; tp = 0.0;
            }
            st.trades_today++;
            return make_sig(dir, 0, 0.0, sl, tp, 0, 0, 0, 0, st.max_hold, 10, st.inst);
        }
        case F14_PROJ_INVERSE_ORB: {
            int om = (st.p0 >= 45) ? 60 : (st.p0 >= 25 ? 30 : 15);
            if ((sess != 2 && sess != 3) || !view.or_formed(om)) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double rng = oh - ol;
            if (rng <= 2.0 * st.inst.tick_size) return sig;
            double sl = rng * st.p1 * idx_scale(view, st.inst);
            if (h >= oh && p_h < oh) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            if (l <= ol && p_l > ol) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, sl * st.p2, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F14_PROJ_YM_ORB_TRAIL_STEP: {
            int om = (st.p0 >= 45) ? 60 : (st.p0 >= 25 ? 30 : 15);
            if ((sess != 2 && sess != 3) || !view.or_formed(om)) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double rng = oh - ol;
            if (rng <= 2.0 * st.inst.tick_size) return sig;
            double sl = rng;
            double tp = sl * st.p4;
            double trig = std::max(2.0 * st.inst.tick_size, rng * st.p1);
            double trail = std::max(2.0 * st.inst.tick_size, rng * st.p2 * (1.0 - 0.5 * st.p3));
            if (h >= oh && p_h < oh) {
                st.trades_today++;
                return make_sig(1, 0, 0.0, sl, tp, trig, trail, 0, 0, st.max_hold, 5, st.inst);
            }
            if (l <= ol && p_l > ol) {
                st.trades_today++;
                return make_sig(-1, 0, 0.0, sl, tp, trig, trail, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F14_LAPTOP_REENTRY_SUITE: {
            int om = (st.p0 >= 45) ? 60 : (st.p0 >= 25 ? 30 : 15);
            if ((sess != 2 && sess != 3) || !view.or_formed(om)) return sig;
            double oh = view.or_high(om), ol = view.or_low(om);
            double rng = oh - ol;
            if (rng <= 2.0 * st.inst.tick_size) return sig;
            double sl = rng * st.p2;
            int8_t dir = 0;
            if (h >= oh && p_h < oh) dir = 1;
            else if (l <= ol && p_l > ol) dir = -1;
            if (dir != 0) {
                // Fade entry #1 if configured
                if (st.entry_num_today == 0 && st.p1 > 0.5) dir = static_cast<int8_t>(-dir);
                st.trades_today++;
                return make_sig(dir, 0, 0.0, sl, sl * st.p3, 0, 0, 0, 0, st.max_hold, 5, st.inst);
            }
            break;
        }
        case F14_KRONOS_SIGNAL: {
            int plen = std::clamp(static_cast<int>(std::round(st.p0)), 2, 4);
            int n = st.kronos_hist.size();
            if (n < 30) return sig;
            double sum_r = 0.0;
            int matches = 0;
            for (int idx = plen; idx < n - 1; ++idx) {
                bool eq = true;
                for (int j = 0; j < plen; ++j) {
                    if (st.kronos_hist.back(j).tok != st.kronos_hist.back(idx - plen + 1 + j).tok) {
                        eq = false; break;
                    }
                }
                if (eq) {
                    sum_r += st.kronos_hist.back(idx - plen).next_ret_atr;
                    matches++;
                }
            }
            if (matches >= 1) {
                double pred = sum_r / static_cast<double>(matches);
                if (pred > st.p1) {
                    st.trades_today++;
                    return make_sig(1, 0, 0.0, st.p2 * cur_atr, st.p3 * cur_atr, 0, 0, 0, 0, st.max_hold, 2, st.inst);
                }
                if (pred < -st.p1) {
                    st.trades_today++;
                    return make_sig(-1, 0, 0.0, st.p2 * cur_atr, st.p3 * cur_atr, 0, 0, 0, 0, st.max_hold, 2, st.inst);
                }
            } else {
                // Token fallback: strong body imbalance token continuation
                double body_a = (c - o) / cur_atr;
                if (std::abs(body_a) > 1.3 * (1.0 + st.p1)) {
                    int8_t dir = (body_a > 0) ? 1 : -1;
                    st.trades_today++;
                    return make_sig(dir, 0, 0.0, st.p2 * cur_atr, st.p3 * cur_atr, 0, 0, 0, 0, st.max_hold, 2, st.inst);
                }
            }
            (void)sc;
            break;
        }
    }
    return sig;
}

template <F14Kind K>
inline void add_f14(std::vector<s546::RegisteredStrategy>& out,
                    const std::string& uid, const std::string& name,
                    const std::string& src, uint8_t sess_mask, bool cross_asset,
                    std::vector<u11::ParamSpec> params) {
    s546::RegisteredStrategy r;
    r.uid = uid;
    r.display_name = name;
    r.super_family = "14_Standalone_Workspaces_NQ_FVG_IV_Kronos";
    r.source_note = src;
    r.fidelity = "EXACT_SOURCE";
    r.needs_cross_asset = cross_asset;
    r.session_mask = sess_mask;
    r.schema.params = std::move(params);
    r.factory = s546::make_factory<F14State>(uid, f14_on_bar, f14_init<K>);
    out.push_back(std::move(r));
}

inline void register_f14(std::vector<s546::RegisteredStrategy>& out) {
    add_f14<F14_FRACTAL_CLUSTER>(out, "fractal_iv_reversal_fractal_cluster", "Fractal / Probability Cluster Reversal", "fractal_iv_reversal/fractal_cluster.py", 0xF, false,
        {p_int("window", 50, 20, 64), p_lin("z_thresh", 2.0, 1.2, 3.0), p_int("swing_min", 1, 1, 3), p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 4.0)});
    add_f14<F14_IV_WALL>(out, "fractal_iv_reversal_iv_wall", "IV-Wall Volatility Estimator Reversal", "fractal_iv_reversal/iv_wall.py", 0xF, false,
        {p_int("vol_estimator", 0, 0, 5), p_int("vol_period", 20, 8, 50), p_lin("wall_mult", 1.0, 0.6, 2.0), p_lin("sl_atr", 1.5, 0.8, 2.5), p_lin("rr", 2.0, 1.0, 4.0)});

    add_f14<F14_INSTA_BOS_FVG>(out, "insta_batch2_bos_fvg", "Insta Batch 2: BOS + FVG Retest", "insta_batch2/bos_fvg.py", 0xF, false,
        {p_int("swing_window", 10, 5, 30), p_int("fvg_lookback", 15, 5, 35), p_lin("rr", 2.0, 1.0, 4.0), p_lin("min_fvg_atr", 0.1, 0.0, 0.5), p_lin("sl_atr", 1.0, 0.6, 2.0)});
    add_f14<F14_INSTA_EMA_VWAP>(out, "insta_batch2_ema_vwap", "Insta Batch 2: 9/20 EMA + VWAP Trend", "insta_batch2/ema_vwap.py", 0xF, false,
        {p_lin("atr_stop_mult", 1.5, 0.8, 3.0), p_lin("atr_target_mult", 3.0, 1.5, 5.0)});
    add_f14<F14_INSTA_LIQ_BOS_VWAP>(out, "insta_batch2_liquidity_bos_vwap", "Insta Batch 2: Liquidity + BOS + VWAP Scalp", "insta_batch2/liquidity_bos_vwap.py", 0xF, false,
        {p_int("liq_lookback", 20, 10, 40), p_int("mom_window", 3, 2, 10), p_lin("risk_multiple", 0.5, 0.25, 0.95), p_lin("sl_atr", 1.2, 0.7, 2.0)});
    add_f14<F14_INSTA_MTF_FVG>(out, "insta_batch2_multi_tf_fvg", "Insta Batch 2: Multi-TF FVG Inversion", "insta_batch2/multi_tf_fvg.py", 0xF, false,
        {p_int("higher_tf", 5, 3, 15), p_int("max_htf_age", 30, 10, 60), p_int("max_ltf_age", 5, 2, 15), p_lin("rr", 1.0, 0.8, 3.0), p_lin("sl_pad_atr", 0.2, 0.05, 0.6)});
    add_f14<F14_INSTA_OPENING_RANGE>(out, "insta_batch2_opening_range", "Insta Batch 2: 15m Opening Range Breakout", "insta_batch2/opening_range.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("buffer_pts", 0.0, 0.0, 8.0), p_lin("rr", 1.0, 0.5, 3.0), p_bool("use_prior_close", false)});
    add_f14<F14_INSTA_PO3_IFVG>(out, "insta_batch2_po3_ifvg", "Insta Batch 2: 4H PO3 + LTF IFVG", "insta_batch2/po3_ifvg.py", 0xF, false,
        {p_lin("min_wick_pct", 0.35, 0.15, 0.55), p_int("ifvg_lookback", 25, 10, 40), p_int("swing_lookback", 18, 8, 35), p_lin("rr", 3.0, 1.5, 5.0)});
    add_f14<F14_INSTA_PO3_SMT>(out, "insta_batch2_po3_smt", "Insta Batch 2: PO3 + SMT Divergence", "insta_batch2/po3_smt.py", 0xF, true,
        {p_int("lookback", 30, 10, 60), p_lin("sl_atr", 0.5, 0.25, 1.5), p_lin("target_r", 2.0, 1.0, 4.0), p_bool("require_smt", true)});
    add_f14<F14_INSTA_PREM_DISC>(out, "insta_batch2_premium_discount", "Insta Batch 2: Premium / Discount Equilibrium", "insta_batch2/premium_discount.py", 0xF, false,
        {p_int("range_lookback", 40, 15, 64), p_lin("discount_th", 0.25, 0.10, 0.45), p_lin("premium_th", 0.75, 0.55, 0.90), p_lin("stop_offset", 0.10, 0.0, 0.40)});
    add_f14<F14_INSTA_SESSION_2CANDLE>(out, "insta_batch2_session_2candle", "Insta Batch 2: Session First-2-Candle Breakout", "insta_batch2/session_2candle.py", 0xF, false,
        {p_lin("min_range_pts", 4.0, 1.0, 20.0), p_lin("rr", 1.5, 0.8, 3.0)});
    add_f14<F14_INSTA_SESSION_LIQ>(out, "insta_batch2_session_liquidity", "Insta Batch 2: NY Open Session Liquidity Sweep + iFVG", "insta_batch2/session_liquidity.py", 0xC, false,
        {p_int("lookback", 60, 20, 64), p_lin("disp_atr", 0.3, 0.1, 1.0), p_lin("rr", 2.0, 1.0, 4.0), p_lin("sl_pad_atr", 0.2, 0.0, 0.6)});

    add_f14<F14_OMAR_NO_WICK_5M>(out, "omar_no_wick_5m_reversal", "Omar Nowick 5m No-Wick A-Setup Reversal", "omar_no_wick/no_wick.py", 0xC, false,
        {p_lin("min_sl_pts", 5.0, 2.0, 15.0), p_lin("sl_breath", 1.10, 1.0, 1.3), p_lin("rr", 1.0, 0.8, 2.5)});

    add_f14<F14_ORB_NQ_ORB>(out, "orb_nq_orb", "ORB NQ Classic Opening Range Breakout", "orb_nq/orb.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("rr", 2.0, 0.8, 4.0), p_lin("stop_mult", 1.0, 0.5, 1.5)});
    add_f14<F14_ORB_NQ_1H_5M_DOLLAR>(out, "orb_nq_orb_1h_5min_dollar", "ORB NQ 1h Signal + 5m Dollar Stop/Target", "orb_nq/orb_1h_5min_dollar.py", 0xC, false,
        {p_int("or_mins", 60, 15, 60, 3), p_lin("dollar_stop", 400.0, 150.0, 800.0), p_lin("dollar_target", 800.0, 300.0, 1600.0)});
    add_f14<F14_ORB_NQ_CLEAN>(out, "orb_nq_orb_clean", "ORB NQ Clean (N x OR Stop, Session Close)", "orb_nq/orb_clean.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("stop_mult", 1.0, 0.25, 1.5)});
    add_f14<F14_ORB_NQ_CLEAN_CLOSE_STOP>(out, "orb_nq_orb_clean_close_stop", "ORB NQ Clean Close-Based Stop", "orb_nq/orb_clean_close_stop.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("stop_mult", 0.5, 0.25, 1.5)});
    add_f14<F14_ORB_NQ_CLEAN_DOLLAR>(out, "orb_nq_orb_clean_dollar", "ORB NQ Clean Dollar Stop/Target", "orb_nq/orb_clean_dollar.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("stop_mult", 0.5, 0.25, 1.5), p_lin("dollar_stop", 500.0, 200.0, 900.0), p_lin("dollar_target", 1000.0, 400.0, 2000.0)});
    add_f14<F14_ORB_NQ_HYBRID_STOP>(out, "orb_nq_orb_hybrid_stop", "ORB NQ Hybrid Soft/Hard Stop", "orb_nq/orb_hybrid_stop.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("soft_mult", 0.5, 0.25, 1.0), p_lin("hard_mult", 1.0, 0.5, 2.0)});
    add_f14<F14_ORB_NQ_PROP_RISK>(out, "orb_nq_orb_prop_risk", "ORB NQ Prop Firm Risk Dollar Stop/Target", "orb_nq/orb_prop_risk.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("stop_mult", 0.5, 0.25, 1.25), p_lin("dollar_stop", 500.0, 200.0, 900.0), p_lin("dollar_target", 1000.0, 400.0, 2000.0)});

    add_f14<F14_PROJ_INVERSE_ORB>(out, "projects_trading_inverse_orb", "Inverse ORB Fade Backtest (YM/DIA)", "live_bots/inverse_orb_backtest.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("sl_or_mult", 1.0, 0.5, 1.5), p_lin("rr", 2.0, 1.0, 4.0)});
    add_f14<F14_PROJ_YM_ORB_TRAIL_STEP>(out, "projects_trading_ym_orb_trail_step", "6-TF YM/DIA ORB with Trail Step Ratchet", "live_bots/ym_orb_retuned.py", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_lin("trig_frac", 0.05, 0.02, 0.30), p_lin("sint", 1.79, 0.5, 2.5), p_lin("lock", 0.90, 0.4, 0.95), p_lin("rr", 5.0, 2.0, 6.0)});

    add_f14<F14_LAPTOP_REENTRY_SUITE>(out, "laptop_reentry_backtest_multi_asset_suite", "Multi-Asset Re-Entry Decomposition & Entry-1 Fade Suite", "test_fade_entry_1_pullback.cpp", 0xC, false,
        {p_int("or_mins", 15, 15, 60, 3), p_bool("fade_entry_1", true), p_lin("sl_or_mult", 1.0, 0.5, 1.5), p_lin("rr", 2.0, 1.0, 4.0), p_int("max_entries", 4, 1, 10)});
    add_f14<F14_KRONOS_SIGNAL>(out, "kronos_signal_foundation_model", "Kronos Tokenized OHLCV Foundation Signal Proxy", "kronos_signal/run_backtest_kronos.py", 0xF, false,
        {p_int("pattern_len", 3, 2, 4), p_lin("pred_atr_th", 0.35, 0.1, 1.0), p_lin("sl_atr", 1.2, 0.7, 2.0), p_lin("tp_atr", 2.4, 1.2, 4.0)});
}

} // namespace b4

inline void register_batch4(std::vector<s546::RegisteredStrategy>& out) {
    b4::register_f06(out);
    b4::register_f08(out);
    b4::register_f11(out);
    b4::register_f14(out);
}

} // namespace s546
