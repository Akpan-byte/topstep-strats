#pragma once
#include "strategy_registry_546.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace s546 {
namespace batch1_detail {

// ----------------------------------------------------------------------------
// Symbol-Agnostic Scaling & Causal Helper Utilities
// ----------------------------------------------------------------------------
inline double inst_ratio(const u11::InstrumentSpec& inst) {
    return inst.baseline_index / 4524.25;
}

inline double sess_index_scale(double sess_open_px, const u11::InstrumentSpec& inst) {
    if (sess_open_px <= 0.0 || inst.baseline_index <= 0.0) return 1.0;
    return std::clamp(sess_open_px / inst.baseline_index, 0.70, 3.00);
}

inline double qtick(double val, double tick) {
    if (tick <= 0.0) return val;
    return std::round(val / tick) * tick;
}

inline double clamp_sl_pts(double pts, const u11::InstrumentSpec& inst) {
    double min_sl = std::max(0.25, inst.tick_size * 4.0);
    return std::max(min_sl, qtick(pts, inst.tick_size));
}

inline double clamp_tp_pts(double pts, const u11::InstrumentSpec& inst) {
    if (pts <= 0.0) return 0.0;
    double min_tp = std::max(0.25, inst.tick_size * 4.0);
    return std::max(min_tp, qtick(pts, inst.tick_size));
}

inline bool sess_mask_allows(int32_t mask, int8_t sess_id) {
    return sess_id >= 0 && sess_id < 4 && ((mask & (1 << sess_id)) != 0);
}

inline u11::ParamSpec p_lin(const char* name, double def_v, double mn, double mx, int grid = 5) {
    return u11::ParamSpec{name, def_v, mn, mx, grid, u11::ParamScale::LINEAR, {}};
}

inline u11::ParamSpec p_int(const char* name, int def_v, int mn, int mx, int grid = 5) {
    return u11::ParamSpec{name, static_cast<double>(def_v), static_cast<double>(mn),
                          static_cast<double>(mx), grid, u11::ParamScale::INT, {}};
}

inline u11::ParamSpec p_disc(const char* name, double def_v, std::vector<double> vals) {
    return u11::ParamSpec{name, def_v, vals.front(), vals.back(),
                          static_cast<int>(vals.size()), u11::ParamScale::DISCRETE, std::move(vals)};
}

inline double get_p(const std::vector<double>& v, size_t idx, double fallback) {
    return (idx < v.size()) ? v[idx] : fallback;
}

// ----------------------------------------------------------------------------
// Strictly Causal Catch-Up Market History (Handles Skipped Outside-Session Bars)
// Never accesses lag > 0; visits each bar at most once in O(1) amortized time.
// ----------------------------------------------------------------------------
struct CausalBarHistory {
    size_t last_bar = SIZE_MAX;
    size_t bars_seen = 0;

    int32_t cur_cme_ymd = -1;
    int32_t cur_day_idx = -1;
    int8_t  cur_sess_id = -1;
    double  sess_open_px = 0.0;
    double  sess_atr_1h = 0.0;

    // Rolling raw 1m windows (capacity 256)
    RollingWindow closes{256};
    RollingWindow highs{256};
    RollingWindow lows{256};
    RollingWindow opens{256};
    RollingWindow vols{64};
    RollingWindow ranges{64};
    RollingWindow vwap_diffs{64};
    RollingWindow atr14_hist{64};
    RollingWindow rsi14_hist{64};
    RollingWindow vel_hist{64};

    // EMAs / Oscillators
    double ema9 = 0.0, ema12 = 0.0, ema20 = 0.0, ema21 = 0.0, ema26 = 0.0, ema50 = 0.0, ema100 = 0.0;
    double macd_sig9 = 0.0, macd_hist = 0.0, prev_macd_hist = 0.0, macd_line = 0.0, prev_macd_line = 0.0, prev_macd_sig = 0.0;
    double prev_c = 0.0, prev_h = 0.0, prev_l = 0.0;
    double rsi14 = 50.0, prev_rsi14 = 50.0, rsi7 = 50.0;
    double avg_g14 = 0.0, avg_l14 = 0.0, avg_g7 = 0.0, avg_l7 = 0.0;
    double sm_tr14 = 0.0, sm_pdm14 = 0.0, sm_ndm14 = 0.0, adx14 = 0.0;
    double atr14_local = 0.0;
    double kdj_k = 50.0, kdj_d = 50.0, kdj_j = 50.0, prev_kdj_k = 50.0, prev_kdj_j = 50.0;

    // HMA(55) support: wma(27) & wma(55) & wma(7)
    double hma55 = 0.0, prev_hma55 = 0.0;
    RollingWindow hma_diff_w{8};

    // Session / Intraday Causal Structures
    double ny_ib_h = -1e18, ny_ib_l = 1e18;
    bool   ny_ib_formed = false;
    double open5m_h = -1e18, open5m_l = 1e18;
    bool   open5m_formed = false;
    double pm_h = -1e18, pm_l = 1e18;
    bool   pm_formed = false;
    double london_h = -1e18, london_l = 1e18;
    bool   has_london = false;
    double prev_london_h = 0.0, prev_london_l = 0.0;
    bool   has_prev_london = false;
    double asian_h = -1e18, asian_l = 1e18;
    bool   has_asian = false;
    double prev_asian_h = 0.0, prev_asian_l = 0.0;
    bool   has_prev_asian = false;
    double cur_sess_h = -1e18, cur_sess_l = 1e18;
    double piv90_h = -1e18, piv90_l = 1e18;
    bool   piv90_formed = false;
    double c_1500 = 0.0;
    bool   has_c_1500 = false;

    // Prior-day VWAP & Volume Profile (POC/VAH/VAL)
    double cur_day_pv = 0.0, cur_day_v = 0.0;
    double prior_day_vwap = 0.0;
    bool   has_prior_day_vwap = false;
    static constexpr int VP_BINS = 40;
    double cur_vp_min = 1e18, cur_vp_max = -1e18;
    double cur_vp_c[VP_BINS]{}, cur_vp_vol[VP_BINS]{};
    int    cur_vp_cnt = 0;
    double pd_poc = 0.0, pd_vah = 0.0, pd_val = 0.0;
    bool   has_pd_vp = false;

    // Prior daily RTH / CME day stats for Crabel (last 8 days)
    double day_o = 0.0, day_h = -1e18, day_l = 1e18, day_c = 0.0;
    RollingWindow daily_h_w{12}, daily_l_w{12}, daily_o_w{12}, daily_c_w{12}, daily_tr_w{12}, daily_stretch_w{12};

    // 5m & 15m causal resampled bars
    int64_t m5_key = -1;
    double m5_o = 0.0, m5_h = -1e18, m5_l = 1e18, m5_c = 0.0, m5_v = 0.0;
    RollingWindow m5_o_w{32}, m5_h_w{32}, m5_l_w{32}, m5_c_w{32}, m5_v_w{32}, m5_rng_w{32};
    double m5_atr14 = 0.0;
    bool m5_just_closed = false;

    int64_t m15_key = -1;
    double m15_h = -1e18, m15_l = 1e18, m15_c = 0.0, m15_v = 0.0;
    double prev_m15_h = 0.0, prev_m15_l = 0.0, prev_m15_c = 0.0, prev_m15_v = 0.0;
    bool has_prev_m15 = false;

    int64_t m2_key = -1;
    double m2_c = 0.0, m2_l = 1e18, m2_h = -1e18, m2_ema20 = 0.0;
    size_t m2_count = 0;
    bool m2_just_closed = false;

    int64_t m10_key = -1;
    double m10_c = 0.0;
    bool m10_just_closed = false;

    // 20-bar VWAP std for Bollinger/VWAP deviation
    bool track_heavy_vp = false;

    static double wma_from_win(const RollingWindow& w, size_t len) {
        if (w.count < len || len == 0) return w.count > 0 ? w.at(0) : 0.0;
        double num = 0.0, den = 0.0;
        for (size_t i = 0; i < len; ++i) {
            double wt = static_cast<double>(len - i);
            num += wt * w.at(i);
            den += wt;
        }
        return (den > 0.0) ? (num / den) : 0.0;
    }

    void finalize_vp_day() {
        if (cur_vp_cnt < 10 || cur_vp_max <= cur_vp_min) return;
        double bins[VP_BINS]{};
        double lo = cur_vp_min;
        double hi = cur_vp_max;
        double step = (hi - lo) / static_cast<double>(VP_BINS);
        if (step <= 1e-9) return;
        double tot_v = 0.0;
        int n_fill = std::min(cur_vp_cnt, VP_BINS);
        for (int k = 0; k < n_fill; ++k) {
            int bi = std::clamp(static_cast<int>((cur_vp_c[k] - lo) / step), 0, VP_BINS - 1);
            bins[bi] += cur_vp_vol[k];
            tot_v += cur_vp_vol[k];
        }
        if (tot_v <= 0.0) return;
        int poc_i = 0;
        for (int b = 1; b < VP_BINS; ++b) {
            if (bins[b] > bins[poc_i]) poc_i = b;
        }
        pd_poc = lo + (static_cast<double>(poc_i) + 0.5) * step;
        int lo_i = poc_i, hi_i = poc_i;
        double va_v = bins[poc_i];
        while (va_v < 0.70 * tot_v && (lo_i > 0 || hi_i < VP_BINS - 1)) {
            double left_v = (lo_i > 0) ? bins[lo_i - 1] : -1.0;
            double right_v = (hi_i < VP_BINS - 1) ? bins[hi_i + 1] : -1.0;
            if (right_v >= left_v) {
                ++hi_i;
                va_v += bins[hi_i];
            } else {
                --lo_i;
                va_v += bins[lo_i];
            }
        }
        pd_val = lo + static_cast<double>(lo_i) * step;
        pd_vah = lo + static_cast<double>(hi_i + 1) * step;
        has_pd_vp = true;
    }

    void step_bar(const u11::RawBar& rb, int16_t et_min, int32_t cme_ymd,
                  double vwap_here, double atr14_here, bool is_current_bar, const u11::CausalBarView& view) {
        double bo = static_cast<double>(rb.o);
        double bh = static_cast<double>(rb.h);
        double bl = static_cast<double>(rb.l);
        double bc = static_cast<double>(rb.c);
        double bv = std::max(0.0, static_cast<double>(rb.v));

        bool new_day = (cme_ymd != cur_cme_ymd);
        if (new_day) {
            if (cur_cme_ymd >= 0 && day_h > day_l) {
                daily_o_w.push(day_o);
                daily_h_w.push(day_h);
                daily_l_w.push(day_l);
                daily_c_w.push(day_c);
                daily_tr_w.push(day_h - day_l);
                daily_stretch_w.push(std::min(day_h - day_o, day_o - day_l));
                if (cur_day_v > 0.0) {
                    prior_day_vwap = cur_day_pv / cur_day_v;
                    has_prior_day_vwap = true;
                }
                if (track_heavy_vp) finalize_vp_day();
            }
            if (has_london) { prev_london_h = london_h; prev_london_l = london_l; has_prev_london = true; }
            if (has_asian) { prev_asian_h = asian_h; prev_asian_l = asian_l; has_prev_asian = true; }
            cur_cme_ymd = cme_ymd;
            day_o = bo; day_h = bh; day_l = bl; day_c = bc;
            ny_ib_h = -1e18; ny_ib_l = 1e18; ny_ib_formed = false;
            open5m_h = -1e18; open5m_l = 1e18; open5m_formed = false;
            pm_h = -1e18; pm_l = 1e18; pm_formed = false;
            london_h = -1e18; london_l = 1e18; has_london = false;
            asian_h = -1e18; asian_l = 1e18; has_asian = false;
            c_1500 = 0.0; has_c_1500 = false;
            cur_day_pv = 0.0; cur_day_v = 0.0;
            cur_vp_min = 1e18; cur_vp_max = -1e18; cur_vp_cnt = 0;
        } else {
            day_h = std::max(day_h, bh);
            day_l = std::min(day_l, bl);
            day_c = bc;
        }

        // Track intraday causal ranges
        if (et_min >= 570 && et_min < 630) {
            ny_ib_h = std::max(ny_ib_h, bh);
            ny_ib_l = std::min(ny_ib_l, bl);
        } else if (et_min >= 630 && ny_ib_h > ny_ib_l) {
            ny_ib_formed = true;
        }
        if (et_min >= 570 && et_min < 575) {
            open5m_h = std::max(open5m_h, bh);
            open5m_l = std::min(open5m_l, bl);
        } else if (et_min >= 575 && open5m_h > open5m_l) {
            open5m_formed = true;
        }
        if (et_min >= 492 && et_min < 552) {
            pm_h = std::max(pm_h, bh);
            pm_l = std::min(pm_l, bl);
        } else if (et_min >= 570 && pm_h > pm_l) {
            pm_formed = true;
        }
        if (et_min >= 180 && et_min < 510) { // London 03:00-08:30 ET
            london_h = std::max(london_h, bh);
            london_l = std::min(london_l, bl);
            has_london = true;
        }
        if (et_min >= 1080 || et_min < 120) {
            asian_h = std::max(asian_h, bh);
            asian_l = std::min(asian_l, bl);
            has_asian = true;
        }
        if (et_min == 900) { c_1500 = bc; has_c_1500 = true; }

        int8_t sid = u11::classify_session(et_min, true);
        if (sid != cur_sess_id || new_day) {
            cur_sess_id = sid;
            sess_open_px = bo;
            sess_atr_1h = atr14_here;
            if (is_current_bar && view.bar_index() > 0) {
                sess_atr_1h = view.atr60(-1);
            }
            cur_sess_h = bh;
            cur_sess_l = bl;
            piv90_h = bh;
            piv90_l = bl;
            piv90_formed = false;
        } else {
            cur_sess_h = std::max(cur_sess_h, bh);
            cur_sess_l = std::min(cur_sess_l, bl);
            if (sid >= 0) {
                int16_t mis = u11::calc_min_in_sess(et_min, sid);
                if (mis < 90) {
                    piv90_h = std::max(piv90_h, bh);
                    piv90_l = std::min(piv90_l, bl);
                } else if (piv90_h > piv90_l) {
                    piv90_formed = true;
                }
            }
        }

        double tp_bar = (bh + bl + bc) / 3.0;
        double vw = (bv > 0.0) ? bv : 1.0;
        cur_day_pv += tp_bar * vw;
        cur_day_v += vw;

        if (track_heavy_vp) {
            cur_vp_min = std::min(cur_vp_min, bc);
            cur_vp_max = std::max(cur_vp_max, bc);
            int slot = cur_vp_cnt % VP_BINS;
            if (cur_vp_cnt < VP_BINS) {
                cur_vp_c[slot] = bc;
                cur_vp_vol[slot] = vw;
            } else {
                cur_vp_c[slot] = 0.5 * (cur_vp_c[slot] + bc);
                cur_vp_vol[slot] += vw;
            }
            ++cur_vp_cnt;
        }

        // Update 1m rolling windows
        opens.push(bo);
        highs.push(bh);
        lows.push(bl);
        closes.push(bc);
        vols.push(bv);
        ranges.push(bh - bl);
        if (vwap_here > 0.0) vwap_diffs.push(bc - vwap_here);

        double tr = bh - bl;
        if (bars_seen > 0) {
            tr = std::max({tr, std::abs(bh - prev_c), std::abs(bl - prev_c)});
            atr14_local = (atr14_local * 13.0 + tr) / 14.0;
        } else {
            atr14_local = tr;
        }
        atr14_hist.push(atr14_local);

        // EMAs & MACD
        if (bars_seen == 0) {
            ema9 = ema12 = ema20 = ema21 = ema26 = ema50 = ema100 = bc;
            macd_line = macd_sig9 = macd_hist = prev_macd_hist = 0.0;
            prev_macd_line = prev_macd_sig = 0.0;
        } else {
            ema9   = (2.0 / 10.0)  * bc + (8.0 / 10.0)   * ema9;
            ema12  = (2.0 / 13.0)  * bc + (11.0 / 13.0)  * ema12;
            ema20  = (2.0 / 21.0)  * bc + (19.0 / 21.0)  * ema20;
            ema21  = (2.0 / 22.0)  * bc + (20.0 / 22.0)  * ema21;
            ema26  = (2.0 / 27.0)  * bc + (25.0 / 27.0)  * ema26;
            ema50  = (2.0 / 51.0)  * bc + (49.0 / 51.0)  * ema50;
            ema100 = (2.0 / 101.0) * bc + (99.0 / 101.0) * ema100;
            prev_macd_line = macd_line;
            prev_macd_sig  = macd_sig9;
            prev_macd_hist = macd_hist;
            macd_line = ema12 - ema26;
            macd_sig9 = (2.0 / 10.0) * macd_line + (8.0 / 10.0) * macd_sig9;
            macd_hist = macd_line - macd_sig9;
        }

        // Wilder RSI14, RSI7, ADX14
        if (bars_seen > 0) {
            double chg = bc - prev_c;
            double g = std::max(0.0, chg);
            double l = std::max(0.0, -chg);
            avg_g14 = (avg_g14 * 13.0 + g) / 14.0;
            avg_l14 = (avg_l14 * 13.0 + l) / 14.0;
            avg_g7  = (avg_g7  * 6.0  + g) / 7.0;
            avg_l7  = (avg_l7  * 6.0  + l) / 7.0;
            prev_rsi14 = rsi14;
            rsi14 = (avg_l14 > 1e-12) ? (100.0 - 100.0 / (1.0 + avg_g14 / avg_l14)) : (avg_g14 > 0.0 ? 100.0 : 50.0);
            rsi7  = (avg_l7  > 1e-12) ? (100.0 - 100.0 / (1.0 + avg_g7  / avg_l7 )) : (avg_g7  > 0.0 ? 100.0 : 50.0);

            double up_move = bh - prev_h;
            double dn_move = prev_l - bl;
            double pdm = (up_move > dn_move && up_move > 0.0) ? up_move : 0.0;
            double ndm = (dn_move > up_move && dn_move > 0.0) ? dn_move : 0.0;
            sm_tr14  = sm_tr14  - sm_tr14  / 14.0 + tr;
            sm_pdm14 = sm_pdm14 - sm_pdm14 / 14.0 + pdm;
            sm_ndm14 = sm_ndm14 - sm_ndm14 / 14.0 + ndm;
            if (sm_tr14 > 1e-9) {
                double pdi = 100.0 * sm_pdm14 / sm_tr14;
                double ndi = 100.0 * sm_ndm14 / sm_tr14;
                double dx  = (pdi + ndi > 1e-9) ? (100.0 * std::abs(pdi - ndi) / (pdi + ndi)) : 0.0;
                adx14 = (bars_seen < 14) ? dx : ((adx14 * 13.0 + dx) / 14.0);
            }
        }
        rsi14_hist.push(rsi14);

        // KDJ(9, 3, 3)
        if (highs.count >= 9) {
            double hh9 = -1e18, ll9 = 1e18;
            for (size_t i = 0; i < 9; ++i) {
                hh9 = std::max(hh9, highs.at(i));
                ll9 = std::min(ll9, lows.at(i));
            }
            double rsv = (hh9 > ll9) ? (100.0 * (bc - ll9) / (hh9 - ll9)) : 50.0;
            prev_kdj_k = kdj_k;
            prev_kdj_j = kdj_j;
            kdj_k = (2.0 / 3.0) * kdj_k + (1.0 / 3.0) * rsv;
            kdj_d = (2.0 / 3.0) * kdj_d + (1.0 / 3.0) * kdj_k;
            kdj_j = 3.0 * kdj_k - 2.0 * kdj_d;
        }

        // HMA(55)
        if (closes.count >= 55) {
            double w27 = wma_from_win(closes, 27);
            double w55 = wma_from_win(closes, 55);
            hma_diff_w.push(2.0 * w27 - w55);
            prev_hma55 = hma55;
            hma55 = wma_from_win(hma_diff_w, 7);
        }

        // Kinetic velocity buffer (k=5)
        if (closes.count > 5) {
            vel_hist.push((bc - closes.at(5)) / 5.0);
        }

        // 5m / 15m / 2m / 10m bucket tracking (strictly causal)
        int64_t ts_sec = rb.ts_sec;
        int64_t k5  = ts_sec / 300;
        int64_t k15 = ts_sec / 900;
        int64_t k2  = ts_sec / 120;
        int64_t k10 = ts_sec / 600;

        m5_just_closed = false;
        if (m5_key < 0) {
            m5_key = k5; m5_o = bo; m5_h = bh; m5_l = bl; m5_c = bc; m5_v = bv;
        } else if (k5 != m5_key) {
            double rng = m5_h - m5_l;
            m5_o_w.push(m5_o); m5_h_w.push(m5_h); m5_l_w.push(m5_l);
            m5_c_w.push(m5_c); m5_v_w.push(m5_v); m5_rng_w.push(rng);
            m5_atr14 = (m5_c_w.count <= 1) ? rng : ((m5_atr14 * 13.0 + rng) / 14.0);
            m5_just_closed = true;
            m5_key = k5; m5_o = bo; m5_h = bh; m5_l = bl; m5_c = bc; m5_v = bv;
        } else {
            m5_h = std::max(m5_h, bh); m5_l = std::min(m5_l, bl); m5_c = bc; m5_v += bv;
        }

        if (m15_key < 0) {
            m15_key = k15; m15_h = bh; m15_l = bl; m15_c = bc; m15_v = bv;
        } else if (k15 != m15_key) {
            prev_m15_h = m15_h; prev_m15_l = m15_l; prev_m15_c = m15_c; prev_m15_v = m15_v;
            has_prev_m15 = true;
            m15_key = k15; m15_h = bh; m15_l = bl; m15_c = bc; m15_v = bv;
        } else {
            m15_h = std::max(m15_h, bh); m15_l = std::min(m15_l, bl); m15_c = bc; m15_v += bv;
        }

        m2_just_closed = false;
        if (m2_key < 0) {
            m2_key = k2; m2_h = bh; m2_l = bl; m2_c = bc;
        } else if (k2 != m2_key) {
            m2_ema20 = (m2_count == 0) ? m2_c : ((2.0 / 21.0) * m2_c + (19.0 / 21.0) * m2_ema20);
            ++m2_count;
            m2_just_closed = true;
            m2_key = k2; m2_h = bh; m2_l = bl; m2_c = bc;
        } else {
            m2_h = std::max(m2_h, bh); m2_l = std::min(m2_l, bl); m2_c = bc;
        }

        m10_just_closed = false;
        if (m10_key < 0) {
            m10_key = k10; m10_c = bc;
        } else if (k10 != m10_key) {
            m10_just_closed = true;
            m10_key = k10; m10_c = bc;
        } else {
            m10_c = bc;
        }

        prev_c = bc; prev_h = bh; prev_l = bl;
        ++bars_seen;
    }

    void sync(const u11::CausalBarView& view, size_t max_warmup = 256) {
        size_t b = view.bar_index();
        size_t start = (last_bar == SIZE_MAX)
            ? (b > max_warmup ? b - max_warmup : 0)
            : (last_bar + 1);
        if (start > b) start = b;
        for (size_t idx = start; idx <= b; ++idx) {
            int lag = -static_cast<int>(b - idx);
            const auto& rb = view[lag];
            int16_t em = 0; int32_t ymd = 0;
            u11::decode_et_and_cme_date(rb.ts_sec, em, ymd);
            double vw = (idx == b) ? view.session_vwap(0) : view.session_vwap(lag);
            double a14 = (idx == b) ? view.atr14(0) : view.atr14(lag);
            step_bar(rb, em, ymd, vw, a14, idx == b, view);
        }
        cur_day_idx = view.cme_day_idx();
        last_bar = b;
    }
};

// ----------------------------------------------------------------------------
// Helper: Core 4 Physics Engines (#159, #151, #157, #155) strictly via CausalBarView
// ----------------------------------------------------------------------------
struct PhysicsSignalOut {
    int8_t dir = 0;
    int    leg_idx = -1; // 0=159, 1=151, 2=157, 3=155
    int    confluence_cnt = 0;
};

inline PhysicsSignalOut eval_physics_core(
    const u11::CausalBarView& view,
    bool allow_159, bool allow_151, bool allow_157, bool allow_155,
    double or_buf_pts,
    double th_159, double th_151, double th_157, double th_155,
    int or_159 = 30, int or_151 = 15, int or_157 = 60, int or_155 = 15
) {
    PhysicsSignalOut res{};
    int8_t s = view.session_id();
    if (s < 0 || view.bar_index() < 12) return res;

    double c_now = static_cast<double>(view[0].c);
    double v_now = (view[0].v > 0.0f) ? static_cast<double>(view[0].v) : 100.0;
    double atr14 = view.atr14(0);
    if (atr14 <= 1e-4) return res;

    auto record_vote = [&](int8_t d, int leg) {
        if (d == 0) return;
        if (res.dir == 0) { res.dir = d; res.leg_idx = leg; }
        if (d == res.dir) res.confluence_cnt++;
    };

    // #159 Hydraulic Shock Pulse (30m OR)
    if (allow_159 && view.or_formed(or_159)) {
        double c2 = static_cast<double>(view[-2].c);
        double pulse = (c_now - c2) * v_now / ((atr14 + 1e-4) * 1000.0);
        double oh = view.or_high(or_159), ol = view.or_low(or_159);
        if (c_now > oh + or_buf_pts && pulse >= th_159) record_vote(1, 0);
        else if (c_now < ol - or_buf_pts && pulse <= -th_159) record_vote(-1, 0);
    }

    // #151 Tsallis Entropy Drop (15m OR)
    if (allow_151 && view.or_formed(or_151)) {
        double c1 = static_cast<double>(view[-1].c);
        double c2 = static_cast<double>(view[-2].c);
        double p1 = std::abs(c_now - c1) / (atr14 + 1e-4) + 1e-4;
        double p2 = std::abs(c1 - c2) / (atr14 + 1e-4) + 1e-4;
        double nrm = p1 + p2;
        p1 /= nrm; p2 /= nrm;
        double tsallis_s = (1.0 - (std::pow(p1, 1.5) + std::pow(p2, 1.5))) / 0.5;
        double oh = view.or_high(or_151), ol = view.or_low(or_151);
        if (tsallis_s <= th_151) {
            if (c_now > oh + or_buf_pts && c_now > c1) record_vote(1, 1);
            else if (c_now < ol - or_buf_pts && c_now < c1) record_vote(-1, 1);
        }
    }

    // #157 Kalman-Volterra Memory Kernel (60m OR)
    if (allow_157 && view.or_formed(or_157)) {
        double conv = 0.0;
        for (int k = 1; k <= 8; ++k) {
            double ck = static_cast<double>(view[-k].c);
            double ckp = static_cast<double>(view[-k - 1].c);
            conv += std::pow(static_cast<double>(k), -0.4) * (ck - ckp);
        }
        double oh = view.or_high(or_157), ol = view.or_low(or_157);
        if (c_now > oh + or_buf_pts && conv >= th_157) record_vote(1, 2);
        else if (c_now < ol - or_buf_pts && conv <= -th_157) record_vote(-1, 2);
    }

    // #155 Asymmetric Clayton Copula Tail Surge (15m OR, strictly dormant in Asian!)
    if (allow_155 && s != 0 && view.or_formed(or_155)) {
        double c1 = static_cast<double>(view[-1].c);
        double u = std::clamp(std::abs(c_now - c1) / (atr14 + 1e-4), 0.001, 0.999);
        double v = std::clamp(v_now / 1000.0, 0.001, 0.999);
        double c_val = std::pow(std::pow(u, -2.0) + std::pow(v, -2.0) - 1.0, -0.5);
        double surge = (c_val / (u * v)) - 1.0;
        if (surge >= th_155) {
            double oh = view.or_high(or_155), ol = view.or_low(or_155);
            if (c_now > oh + or_buf_pts) record_vote(1, 3);
            else if (c_now < ol - or_buf_pts) record_vote(-1, 3);
        }
    }
    return res;
}

// ============================================================================
// ARCHETYPE 1: Flagship Multi-Leg Physics & Experimental Mechanism Suites
// Variants:
//   0 = 000_grand_champion_trio       (159+151+157, SL=36, TP=60, R 2.5/1.25)
//   1 = 001_champion_quad             (159+151+157+155, SL=36, TP=60, R 1.5/0.75)
//   2 = 002_matrix_12leg_decoupled    (session SL 5/6.5/8, R 0.75/1.0/1.25 / 0.5)
//   3 = 002_matrix_12leg_interbar     (#31412 Mode2 +0.50pt confirm, R 0.75/1.0/1.25)
//   4 = 002_matrix_12leg_intrabar     (#31395 FastConfirm 0.50pt)
//   5 = 002_matrix_12leg_safety_king  (#389329964 T2.00/D0.50)
//   6 = 002_matrix_12leg_velocity_dem (#1551249409 high velocity)
//   7 = 003_velocity_demon_dalton_ib  (Velocity Demon + Dalton IB 10:30-15:30)
//   8 = 008_booster_champion          (Physics + VWAP pullback + Dip Magnet P0)
//   9 = exp_12cell_reentry_vwap_swing (Physics + Swing15m/EMA/VWAP pullback reentry)
//  10 = exp_amd_architecture          (Physics + AMD Judas sweep & reclaim gate)
//  11 = exp_magnet_and_dip_reclaim    (Physics + Dip reclaim magnet toward P0)
//  12 = exp_phase1_thursday_causal    (Configurable OR/threshold/SL/ratchet physics)
//  13 = exp_social_alpha_trio_hybrids (Velocity Demon + Social Alpha Trio R46/R29/R39)
//  14 = exp_squeeze_confluences       (BB Squeeze duration + >=2 engine confluence)
//  15 = exp_staging_overlays_apex     (Mode2 confirm + selectivity + granular ratchet)
// ============================================================================
struct PhysicsFlagshipState {
    u11::InstrumentSpec inst;
    int variant = 0;
    int32_t session_mask = 0xF;
    CausalBarHistory hist;

    // Tunable parameters
    double sl_base = 8.0;
    double tp_base = 60.0;
    double r_trig = 1.25;
    double r_trail = 0.50;
    double or_buf = 1.0;
    double th_159 = 1.50;
    double th_151 = 0.80;
    double th_157 = 3.00;
    double th_155 = 1.50;
    double extra_p = 0.50; // Mode2 confirm / magnet dip / squeeze bars / AMD sweep
    int    max_entries = 12;
    int    cooldown = 2;

    // Stateful tracking for Dalton/Booster/Magnets/Reentries
    int32_t dalton_day = -1;
    int     dalton_trades = 0;
    bool    was_in_pos = false;
    int8_t  last_pos_dir = 0;
    double  last_entry_px = 0.0;
    double  last_peak_fav = 0.0;
    bool    reentry_armed = false;
    int8_t  reentry_dir = 0;
    size_t  reentry_expire_bar = 0;
    int     reentry_used = 0;
    bool    magnet_armed = false;
    int8_t  magnet_orig_dir = 0;
    double  magnet_p0 = 0.0;
};

inline void init_physics_impl(PhysicsFlagshipState& st, const std::vector<double>& p,
                              const u11::InstrumentSpec* spec, int variant) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.variant = variant;
    st.hist = CausalBarHistory{};
    st.sl_base     = get_p(p, 0, 8.0);
    st.tp_base     = get_p(p, 1, 60.0);
    st.r_trig      = get_p(p, 2, 1.25);
    st.r_trail     = get_p(p, 3, 0.50);
    st.or_buf      = get_p(p, 4, 1.0);
    st.th_159      = get_p(p, 5, 1.50);
    st.th_151      = get_p(p, 6, 0.80);
    st.th_157      = get_p(p, 7, 3.00);
    st.extra_p     = get_p(p, 8, 0.50);
    st.max_entries = static_cast<int>(std::round(get_p(p, 9, 12.0)));
    st.session_mask = (variant <= 6 || (variant >= 9 && variant <= 11) || variant >= 14) ? 0x7 : 0xF;
}

template <int Variant>
inline void init_physics_v(PhysicsFlagshipState& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_physics_impl(st, p, s, Variant);
}

inline u11::PluginSignal physics_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<PhysicsFlagshipState*>(state_ptr);
    st->hist.sync(view, 256);

    u11::PluginSignal sig{};
    int8_t s_id = view.session_id();
    if (view.is_new_sess()) {
        st->reentry_armed = false;
        st->reentry_used = 0;
        st->magnet_armed = false;
    }
    if (view.cme_day_idx() != st->dalton_day) {
        st->dalton_day = view.cme_day_idx();
        st->dalton_trades = 0;
    }

    double c_now = static_cast<double>(view[0].c);
    double h_now = static_cast<double>(view[0].h);
    double l_now = static_cast<double>(view[0].l);
    double o_now = static_cast<double>(view[0].o);

    // Track position lifecycle causally for Booster/Magnet/Reentry states
    if (view.in_position) {
        if (!st->was_in_pos) {
            st->was_in_pos = true;
            st->last_entry_px = c_now;
            st->last_peak_fav = 0.0;
        } else {
            double fav = (st->last_pos_dir >= 0) ? (h_now - st->last_entry_px) : (st->last_entry_px - l_now);
            st->last_peak_fav = std::max(st->last_peak_fav, fav);
        }
        return sig;
    } else if (st->was_in_pos) {
        st->was_in_pos = false;
        bool exited_adverse = (st->last_pos_dir == 1 && c_now < st->last_entry_px) ||
                              (st->last_pos_dir == -1 && c_now > st->last_entry_px);
        if (exited_adverse && (st->variant == 8 || st->variant == 11)) {
            st->magnet_armed = true;
            st->magnet_orig_dir = st->last_pos_dir;
            st->magnet_p0 = st->last_entry_px;
        }
        if ((st->variant == 8 || st->variant == 9) && st->reentry_used < 1) {
            st->reentry_armed = true;
            st->reentry_dir = st->last_pos_dir;
            st->reentry_expire_bar = view.bar_index() + 30;
        }
    }

    if (!sess_mask_allows(st->session_mask, s_id)) return sig;
    if (view.sess_entries >= st->max_entries) return sig;

    const double ir = inst_ratio(st->inst);
    const double buf_scaled = st->or_buf * ir;

    // Variant 8 / 11: Dip Magnet recovery toward P0 (strictly structural geometry!)
    if ((st->variant == 8 || st->variant == 11) && st->magnet_armed) {
        double dip_thresh = ((st->variant == 8) ? 8.0 : st->extra_p) * ir;
        double sl_d = clamp_sl_pts(12.0 * ir, st->inst);
        if (st->magnet_orig_dir == 1 && c_now <= st->magnet_p0 - dip_thresh) {
            double tp_d = clamp_tp_pts(st->magnet_p0 - c_now, st->inst);
            // Structural invariant: target > stop -> direction = +1
            double est_stop = c_now - sl_d;
            double est_targ = c_now + tp_d;
            if (u11::validate_structural_side(1, est_stop, est_targ)) {
                st->magnet_armed = false;
                st->last_pos_dir = 1;
                sig.direction = 1;
                sig.order_type = 0;
                sig.sl_pts = sl_d;
                sig.tp_pts = tp_d;
                sig.be_trig_pts = 0.5 * tp_d;
                sig.be_offset_pts = 0.25 * ir;
                sig.max_hold_bars = 35;
                sig.cooldown_bars = 1;
                return sig;
            }
        } else if (st->magnet_orig_dir == -1 && c_now >= st->magnet_p0 + dip_thresh) {
            double tp_d = clamp_tp_pts(c_now - st->magnet_p0, st->inst);
            double est_stop = c_now + sl_d;
            double est_targ = c_now - tp_d;
            if (u11::validate_structural_side(-1, est_stop, est_targ)) {
                st->magnet_armed = false;
                st->last_pos_dir = -1;
                sig.direction = -1;
                sig.order_type = 0;
                sig.sl_pts = sl_d;
                sig.tp_pts = tp_d;
                sig.be_trig_pts = 0.5 * tp_d;
                sig.be_offset_pts = 0.25 * ir;
                sig.max_hold_bars = 35;
                sig.cooldown_bars = 1;
                return sig;
            }
        }
    }

    // Variant 8 / 9: Pullback Re-entry (VWAP std / Swing15m / EMA9-21)
    if ((st->variant == 8 || st->variant == 9) && st->reentry_armed && view.bar_index() <= st->reentry_expire_bar) {
        double vwap = view.session_vwap(0);
        double vstd = std::max(0.5 * ir, view.session_vwap_std(0));
        bool vwap_pb = (std::abs(c_now - vwap) <= 1.0 * vstd);
        bool ema_pb = (st->reentry_dir == 1 && l_now <= st->hist.ema21 && c_now > st->hist.ema9) ||
                      (st->reentry_dir == -1 && h_now >= st->hist.ema21 && c_now < st->hist.ema9);
        if (vwap_pb || (st->variant == 9 && ema_pb)) {
            st->reentry_armed = false;
            st->reentry_used++;
            double sess_sl = (s_id == 0) ? 5.0 : ((s_id == 1) ? 6.5 : st->sl_base);
            sig.direction = st->reentry_dir;
            sig.order_type = 0;
            sig.sl_pts = clamp_sl_pts(sess_sl * ir, st->inst);
            sig.tp_pts = clamp_tp_pts(st->tp_base * ir, st->inst);
            sig.ratchet_trig_pts = std::max(st->inst.tick_size, st->r_trig * ir);
            sig.ratchet_trail_pts = std::max(st->inst.tick_size, st->r_trail * ir);
            sig.max_hold_bars = 45;
            sig.cooldown_bars = 2;
            st->last_pos_dir = sig.direction;
            return sig;
        }
    }

    // Core physics signal evaluation
    bool allow_155 = (st->variant != 0); // Grand Champion Trio excludes #155
    int or_w_override = (st->variant == 12) ? static_cast<int>(std::round(st->extra_p)) : 0;
    auto phys = (or_w_override > 0)
        ? eval_physics_core(view, true, true, true, allow_155, buf_scaled, st->th_159, st->th_151, st->th_157, st->th_155,
                            or_w_override, or_w_override, or_w_override, or_w_override)
        : eval_physics_core(view, true, true, true, allow_155, buf_scaled, st->th_159, st->th_151, st->th_157, st->th_155);

    // Variant 14: Squeeze + Confluence Gate
    if (st->variant == 14) {
        if (st->hist.closes.count >= 25) {
            double mean20 = 0.0, sq20 = 0.0;
            for (size_t i = 0; i < 20; ++i) {
                double x = st->hist.closes.at(i);
                mean20 += x; sq20 += x * x;
            }
            mean20 /= 20.0;
            double std20 = std::sqrt(std::max(0.0, sq20 / 20.0 - mean20 * mean20));
            double atr14 = view.atr14(0);
            bool in_squeeze = (2.0 * std20 < 1.5 * atr14);
            int min_eng = static_cast<int>(std::round(st->extra_p));
            if (!in_squeeze && phys.confluence_cnt < std::max(1, min_eng)) {
                phys.dir = 0;
            }
        }
    }

    // Variant 10: AMD Judas Sweep & Reclaim Gate
    if (st->variant == 10 && phys.dir != 0) {
        if (!view.or_formed(30)) {
            phys.dir = 0;
        } else {
            double oh = view.or_high(30), ol = view.or_low(30);
            double sw = st->extra_p * ir;
            bool ok_long = (phys.dir == 1 && l_now <= ol + sw && c_now > ol);
            bool ok_short = (phys.dir == -1 && h_now >= oh - sw && c_now < oh);
            if (!ok_long && !ok_short && phys.confluence_cnt < 2) phys.dir = 0;
        }
    }

    // Variant 13: Hybrid Velocity Demon + Social Alpha Trio (R46/R29/R39)
    if (st->variant == 13 && phys.dir == 0 && st->hist.closes.count >= 241) {
        double h240 = -1e18, l240 = 1e18, h60 = -1e18, l60 = 1e18, h15 = -1e18, l15 = 1e18;
        for (size_t i = 1; i <= 240; ++i) {
            double hi = st->hist.highs.at(i), lo = st->hist.lows.at(i);
            h240 = std::max(h240, hi); l240 = std::min(l240, lo);
            if (i <= 60) { h60 = std::max(h60, hi); l60 = std::min(l60, lo); }
            if (i <= 15) { h15 = std::max(h15, hi); l15 = std::min(l15, lo); }
        }
        int8_t sa_dir = 0;
        if (h_now > h240 && c_now < o_now) sa_dir = -1;
        else if (l_now < l240 && c_now > o_now) sa_dir = 1;
        else if (h_now > h60 && c_now < h60) sa_dir = -1;
        else if (l_now < l60 && c_now > l60) sa_dir = 1;
        else if (h_now > h15 && c_now < o_now) sa_dir = -1;
        else if (l_now < l15 && c_now > o_now) sa_dir = 1;
        if (sa_dir != 0) {
            sig.direction = sa_dir;
            sig.order_type = 0;
            sig.sl_pts = clamp_sl_pts(12.0 * ir, st->inst);
            sig.tp_pts = clamp_tp_pts(24.0 * ir, st->inst);
            sig.be_trig_pts = 6.0 * ir;
            sig.be_offset_pts = 2.0 * ir;
            sig.max_hold_bars = 60;
            sig.cooldown_bars = 2;
            st->last_pos_dir = sa_dir;
            return sig;
        }
    }

    // Variant 7: Unified Velocity Demon + Dalton IB (10:30-15:30 ET)
    if (st->variant == 7 && phys.dir == 0 && st->hist.ny_ib_formed && st->dalton_trades < 2) {
        int16_t em = view.et_mins();
        if (em >= 630 && em < 930 && view.bar_index() >= 1) {
            double ph = static_cast<double>(view[-1].h);
            double pl = static_cast<double>(view[-1].l);
            double pc = static_cast<double>(view[-1].c);
            double tol = 2.0 * ir;
            int8_t d_dir = 0;
            if (ph >= st->hist.ny_ib_h - tol && pc < st->hist.ny_ib_h) d_dir = -1;
            else if (pl <= st->hist.ny_ib_l + tol && pc > st->hist.ny_ib_l) d_dir = 1;
            if (d_dir != 0) {
                st->dalton_trades++;
                st->last_pos_dir = d_dir;
                sig.direction = d_dir;
                sig.order_type = 0;
                sig.sl_pts = clamp_sl_pts(12.0 * ir, st->inst);
                sig.tp_pts = clamp_tp_pts(20.0 * ir, st->inst);
                sig.be_trig_pts = 6.0 * ir;
                sig.be_offset_pts = 2.0 * ir;
                sig.ratchet_trig_pts = 6.0 * ir;
                sig.ratchet_trail_pts = 2.0 * ir;
                sig.max_hold_bars = 90;
                sig.cooldown_bars = 3;
                return sig;
            }
        }
    }

    if (phys.dir == 0) return sig;

    // Session-decoupled SL & Ratchet for Matrix-12Leg / Velocity Demon / Booster
    double sl_nq = st->sl_base;
    double rt_nq = st->r_trig;
    double rd_nq = st->r_trail;
    if (st->variant >= 2 && st->variant <= 8) {
        if (st->variant == 5) { // Safety King flat
            sl_nq = st->sl_base;
            rt_nq = st->r_trig;
            rd_nq = st->r_trail;
        } else {
            if (s_id == 0) { sl_nq = 5.0; rt_nq = std::min(st->r_trig, 0.75); }
            else if (s_id == 1) { sl_nq = 6.5; rt_nq = std::min(st->r_trig, 1.00); }
            else { sl_nq = st->sl_base; }
        }
    }

    st->last_pos_dir = phys.dir;
    sig.direction = phys.dir;

    // Mode 2 Delayed Confirmation (+0.50pt / +1.00pt stop-entry) for Matrix12 Interbar / Staging
    if ((st->variant == 3 || st->variant == 4 || st->variant == 15) && st->extra_p > 0.0) {
        sig.order_type = 1;
        double conf = st->extra_p * ir;
        sig.trigger_px = (phys.dir == 1) ? (c_now + conf) : (c_now - conf);
    } else {
        sig.order_type = 0;
    }

    sig.sl_pts = clamp_sl_pts(sl_nq * ir, st->inst);
    sig.tp_pts = clamp_tp_pts(st->tp_base * ir, st->inst);
    sig.ratchet_trig_pts = (rt_nq > 0.0) ? std::max(st->inst.tick_size, rt_nq * ir) : 0.0;
    sig.ratchet_trail_pts = (rd_nq > 0.0) ? std::max(st->inst.tick_size, rd_nq * ir) : 0.0;
    if (st->variant == 8) {
        sig.be_trig_pts = 2.50 * ir;
        sig.be_offset_pts = 2.25 * ir;
    }
    sig.max_hold_bars = 60;
    sig.cooldown_bars = st->cooldown;
    return sig;
}

// ============================================================================
// ARCHETYPE 2: Dalton IB + Social Alpha Vault Snipers + Legacy Silver Bullet
// Variants:
//   0 = 004_dalton_ib_midpoint_fade_execution
//   1 = 005_reel_46_trend_continuation_sniper (10:00-12:00 ET)
//   2 = 006_reel_29_orb_momentum_sniper (09:45-11:30 ET)
//   3 = 007_reel_39_asian_midpoint_sniper (20:00-01:00 ET)
//   4 = social_alpha_cheatcodes_4h_fractal (NY 09:30-16:00)
//   5 = social_alpha_creative_confirmation (NY 09:30-16:00)
//   6 = social_alpha_dodgy_resting_sweep (NY 09:30-16:00)
//   7 = social_alpha_portfolio_apex_1m_trio (R46+R29+R39 1m)
//   8 = social_alpha_portfolio_best_tf_champions (R46+R29+R39)
//   9 = social_alpha_portfolio_mega_stack_1m_3m_5m (R46+R29+R39 1m/5m)
//  10 = exp_legacy_portfolio_stacks_paper1_paper2 (London+NY Silver Bullet)
// ============================================================================
struct SniperVaultState {
    u11::InstrumentSpec inst;
    int variant = 0;
    int32_t session_mask = 0xC;
    CausalBarHistory hist;

    double sl_base = 12.0;
    double rr_mult = 2.0;
    double be_trig_r = 0.50;
    double be_off_pts = 2.0;
    double tol_pts = 2.0;
    int    lookback = 240;
    int    max_per_day = 3;

    int32_t cur_day = -1;
    int     trades_today = 0;
};

inline void init_sniper_impl(SniperVaultState& st, const std::vector<double>& p,
                             const u11::InstrumentSpec* spec, int variant) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.variant = variant;
    st.hist = CausalBarHistory{};
    st.sl_base    = get_p(p, 0, 12.0);
    st.rr_mult    = get_p(p, 1, 2.0);
    st.be_trig_r  = get_p(p, 2, 0.50);
    st.be_off_pts = get_p(p, 3, 2.0);
    st.lookback   = static_cast<int>(std::round(get_p(p, 4, 240.0)));
    st.max_per_day = static_cast<int>(std::round(get_p(p, 5, 3.0)));
    if (variant == 3) st.session_mask = 0x1;
    else if (variant == 1 || variant == 2) st.session_mask = 0x4;
    else if (variant == 10) st.session_mask = 0xE;
    else st.session_mask = 0xC;
}

template <int Variant>
inline void init_sniper_v(SniperVaultState& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_sniper_impl(st, p, s, Variant);
}

inline u11::PluginSignal sniper_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<SniperVaultState*>(state_ptr);
    st->hist.sync(view, 256);

    u11::PluginSignal sig{};
    if (view.cme_day_idx() != st->cur_day) {
        st->cur_day = view.cme_day_idx();
        st->trades_today = 0;
    }
    if (view.in_position) return sig;
    if (!sess_mask_allows(st->session_mask, view.session_id())) return sig;
    if (st->trades_today >= st->max_per_day) return sig;

    const double ir = inst_ratio(st->inst);
    const int16_t em = view.et_mins();
    const double c = static_cast<double>(view[0].c);
    const double o = static_cast<double>(view[0].o);
    const double h = static_cast<double>(view[0].h);
    const double l = static_cast<double>(view[0].l);
    int8_t dir = 0;

    if (st->variant == 0) { // Dalton IB Midpoint Fade (10:30-15:30 ET)
        if (!st->hist.ny_ib_formed || em < 630 || em >= 930 || view.bar_index() < 1) return sig;
        double ph = static_cast<double>(view[-1].h);
        double pl = static_cast<double>(view[-1].l);
        double pc = static_cast<double>(view[-1].c);
        double tol = st->tol_pts * ir;
        if (ph >= st->hist.ny_ib_h - tol && pc < st->hist.ny_ib_h) dir = -1;
        else if (pl <= st->hist.ny_ib_l + tol && pc > st->hist.ny_ib_l) dir = 1;
    } else if (st->variant == 10) { // Legacy Silver Bullet Stack (London + NY)
        if (!st->hist.has_prev_m15) return sig;
        if (h > st->hist.prev_m15_h && c < st->hist.prev_m15_h && c < o) dir = -1;
        else if (l < st->hist.prev_m15_l && c > st->hist.prev_m15_l && c > o) dir = 1;
    } else {
        // Time-window filters for Vault Snipers 005/006/007
        if (st->variant == 1 && (em < 600 || em >= 720)) return sig; // 10:00-12:00 ET
        if (st->variant == 2 && (em < 585 || em >= 690)) return sig; // 09:45-11:30 ET
        if (st->variant == 3 && (em < 1200 && em >= 60)) return sig; // 20:00-01:00 ET

        auto check_sweep = [&](int lb, bool reject_level) -> int8_t {
            if (st->hist.highs.count <= static_cast<size_t>(lb)) return 0;
            double hh = -1e18, ll = 1e18;
            for (size_t i = 1; i <= static_cast<size_t>(lb); ++i) {
                hh = std::max(hh, st->hist.highs.at(i));
                ll = std::min(ll, st->hist.lows.at(i));
            }
            if (reject_level) {
                if (h > hh && c < hh) return -1;
                if (l < ll && c > ll) return 1;
            } else {
                if (h > hh && c < o) return -1;
                if (l < ll && c > o) return 1;
            }
            return 0;
        };

        if (st->variant == 1 || st->variant == 4) {
            dir = check_sweep(std::clamp(st->lookback, 30, 240), false);
        } else if (st->variant == 2 || st->variant == 6) {
            dir = check_sweep(std::clamp(st->lookback, 15, 120), true);
        } else if (st->variant == 3 || st->variant == 5) {
            dir = check_sweep(std::clamp(st->lookback, 5, 60), false);
        } else {
            // Portfolio ensembles (057/058/059): R46 + R29 + R39
            dir = check_sweep(240, false);
            if (dir == 0) dir = check_sweep(60, true);
            if (dir == 0) dir = check_sweep(15, false);
        }
    }

    if (dir == 0) return sig;
    st->trades_today++;
    double sl = clamp_sl_pts(st->sl_base * ir, st->inst);
    double tp = clamp_tp_pts(sl * st->rr_mult, st->inst);
    sig.direction = dir;
    sig.order_type = 0;
    sig.sl_pts = sl;
    sig.tp_pts = tp;
    sig.be_trig_pts = st->be_trig_r * sl;
    sig.be_offset_pts = st->be_off_pts * ir;
    sig.ratchet_trig_pts = st->be_trig_r * sl;
    sig.ratchet_trail_pts = std::max(st->inst.tick_size, 0.25 * sl);
    sig.max_hold_bars = 120;
    sig.cooldown_bars = 2;
    return sig;
}

// ============================================================================
// ARCHETYPE 3: ORB & Pullback Generations (19 UIDs)
// Variants:
//   0 = cimi_orb (OR30 5m close / OR15)
//   1 = crabel_orb_stretch (ID/NR4/NR7/IDNR4 + Stretch OCO stop-entry)
//   2 = flexing_joe_orb (OR30 -> 10m break -> 2m EMA20 pullback)
//   3 = kasen_midpoint_retest (OR break -> midpoint retest)
//   4 = orb_v5 / original_orb / live_runner_v1 (index-scaled resting ORB + R-step)
//   5 = orb_v5_5 / orb_cooptimized (index-scaled + VCS/elastic snap)
//   6 = orb_v6 (1H pre-market ATR power-law buffer + skip_first)
//   7 = orb_v6_5 / trade_slices / eval_speed / funded_payout (ATR1h + D34 elastic)
//   8 = orb_pullback_scalper / pullback_v2 / pullback_v2_5 / pullback_coopt
//   9 = kernel_reel_7 (Usman Ashraf 15m OR, 5m close, opposite OR SL, 2:1 RR)
// ============================================================================
struct OrbGenState {
    u11::InstrumentSpec inst;
    int variant = 4;
    int32_t session_mask = 0xC;
    CausalBarHistory hist;

    int    or_mins = 15;
    double buf_base = 20.0;
    double sl_base = 36.0;
    double rr_tp = 0.0;
    double r_trig_r = 0.50;
    double r_trail_r = 0.25;
    int    max_entries = 12;
    int    min_entry_idx = 0; // for skip_first_trade / trade_slices
    int    max_entry_idx = 12;
    double stretch_mult = 1.0;

    int32_t cur_day = -1;
    int8_t  cur_sess = -1;
    int     breakout_seen_dir = 0;
    bool    flex_break_long = false, flex_break_short = false;
    bool    kasen_break_long = false, kasen_break_short = false;
    bool    pb_armed_long = false, pb_armed_short = false;
    int     shadow_breaks = 0;
    bool    last_was_high = false;
};

inline void init_orb_impl(OrbGenState& st, const std::vector<double>& p,
                          const u11::InstrumentSpec* spec, int variant) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.variant = variant;
    st.hist = CausalBarHistory{};
    st.or_mins      = static_cast<int>(std::round(get_p(p, 0, 15.0)));
    st.buf_base     = get_p(p, 1, 25.0);
    st.sl_base      = get_p(p, 2, 36.0);
    st.rr_tp        = get_p(p, 3, 0.0);
    st.r_trig_r     = get_p(p, 4, 0.50);
    st.r_trail_r    = get_p(p, 5, 0.25);
    st.max_entries  = static_cast<int>(std::round(get_p(p, 6, 12.0)));
    st.min_entry_idx = static_cast<int>(std::round(get_p(p, 7, 0.0)));
    st.stretch_mult = get_p(p, 8, 1.0);
    st.session_mask = (variant >= 4 && variant <= 7) ? 0xF : 0xC;
    if (variant == 1 || variant == 2 || variant == 9) st.session_mask = 0x4;
}

template <int Variant>
inline void init_orb_v(OrbGenState& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_orb_impl(st, p, s, Variant);
}

inline u11::PluginSignal orb_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<OrbGenState*>(state_ptr);
    st->hist.sync(view, 256);

    u11::PluginSignal sig{};
    int8_t s_id = view.session_id();
    if (view.cme_day_idx() != st->cur_day || s_id != st->cur_sess || view.is_new_sess()) {
        st->cur_day = view.cme_day_idx();
        st->cur_sess = s_id;
        st->breakout_seen_dir = 0;
        st->flex_break_long = st->flex_break_short = false;
        st->kasen_break_long = st->kasen_break_short = false;
        st->pb_armed_long = st->pb_armed_short = false;
        st->shadow_breaks = 0;
    }

    if (!sess_mask_allows(st->session_mask, s_id)) return sig;
    if (view.in_position || view.sess_entries >= st->max_entries) return sig;

    const double ir = inst_ratio(st->inst);
    const double scale = sess_index_scale(st->hist.sess_open_px, st->inst);
    const double c = static_cast<double>(view[0].c);
    const double h = static_cast<double>(view[0].h);
    const double l = static_cast<double>(view[0].l);
    const int16_t em = view.et_mins();

    // Variant 1: Toby Crabel ORB + Stretch (ID / NR4 / NR7 / IDNR4)
    if (st->variant == 1) {
        if (st->hist.daily_tr_w.count < 7 || em >= 615 || em < 570) return sig;
        if (view.sess_entries >= 1) return sig;
        double y_tr = st->hist.daily_tr_w.at(0);
        double min6 = 1e18;
        for (size_t i = 1; i <= 6; ++i) min6 = std::min(min6, st->hist.daily_tr_w.at(i));
        bool is_id = (st->hist.daily_h_w.at(0) < st->hist.daily_h_w.at(1)) &&
                     (st->hist.daily_l_w.at(0) > st->hist.daily_l_w.at(1));
        bool is_nr = (y_tr < min6);
        if (!is_id && !is_nr) return sig;

        double stretch = st->hist.daily_stretch_w.mean() * st->stretch_mult;
        if (stretch <= st->inst.tick_size) return sig;
        double buy_stop = st->hist.sess_open_px + stretch;
        double sell_stop = st->hist.sess_open_px - stretch;
        double sl_d = clamp_sl_pts(2.0 * stretch, st->inst);
        if (std::abs(c - buy_stop) <= std::abs(c - sell_stop)) {
            sig.direction = 1; sig.trigger_px = buy_stop;
        } else {
            sig.direction = -1; sig.trigger_px = sell_stop;
        }
        sig.order_type = 1;
        sig.sl_pts = sl_d;
        sig.tp_pts = (st->rr_tp > 0.0) ? clamp_tp_pts(st->rr_tp * sl_d, st->inst) : 0.0;
        sig.max_hold_bars = 360;
        sig.cooldown_bars = 1;
        return sig;
    }

    int or_w = (st->or_mins == 15 || st->or_mins == 30 || st->or_mins == 45 || st->or_mins == 60) ? st->or_mins : 15;
    if (!view.or_formed(or_w)) return sig;

    const double oh = view.or_high(or_w);
    const double ol = view.or_low(or_w);
    const double or_span = std::max(st->inst.tick_size * 4.0, oh - ol);
    const double or_mid = 0.5 * (oh + ol);

    // Variant 0: Cimi ORB (5m close outside OR30/OR15)
    if (st->variant == 0) {
        if (!st->hist.m5_just_closed || view.sess_entries >= 1) return sig;
        if (c > oh) {
            double sl_d = clamp_sl_pts((or_w == 30) ? (c - l) : (c - ol), st->inst);
            sig.direction = 1; sig.order_type = 0; sig.sl_pts = sl_d;
            sig.tp_pts = clamp_tp_pts(sl_d * st->rr_tp, st->inst);
            sig.max_hold_bars = 180;
        } else if (or_w == 15 && c < ol) {
            double sl_d = clamp_sl_pts(oh - c, st->inst);
            sig.direction = -1; sig.order_type = 0; sig.sl_pts = sl_d;
            sig.tp_pts = clamp_tp_pts(sl_d * st->rr_tp, st->inst);
            sig.max_hold_bars = 180;
        }
        return sig;
    }

    // Variant 9: Usman Ashraf 15m Classic ORB (5m close outside OR15, SL opposite OR capped, 2:1 RR)
    if (st->variant == 9) {
        if (!st->hist.m5_just_closed || view.sess_entries >= 1) return sig;
        double cap_sl = st->sl_base * ir * scale;
        if (c > oh) {
            double sl_d = clamp_sl_pts(std::min(cap_sl, c - ol), st->inst);
            sig.direction = 1; sig.order_type = 0; sig.sl_pts = sl_d;
            sig.tp_pts = clamp_tp_pts(sl_d * st->rr_tp, st->inst);
            sig.max_hold_bars = 240;
        } else if (c < ol) {
            double sl_d = clamp_sl_pts(std::min(cap_sl, oh - c), st->inst);
            sig.direction = -1; sig.order_type = 0; sig.sl_pts = sl_d;
            sig.tp_pts = clamp_tp_pts(sl_d * st->rr_tp, st->inst);
            sig.max_hold_bars = 240;
        }
        return sig;
    }

    // Variant 2: Flexing Joe ORB (OR30 -> 10m close outside -> 2m EMA20 pullback)
    if (st->variant == 2) {
        if (st->hist.m10_just_closed) {
            if (c > oh) st->flex_break_long = true;
            else if (c < ol) st->flex_break_short = true;
        }
        if (st->hist.m2_just_closed && st->hist.m2_count >= 20) {
            if (st->flex_break_long && l <= st->hist.m2_ema20 && c > st->hist.m2_ema20) {
                st->flex_break_long = false;
                double sl_d = clamp_sl_pts(std::min(st->sl_base * ir * scale, or_span), st->inst);
                sig.direction = 1; sig.order_type = 0; sig.sl_pts = sl_d;
                sig.tp_pts = clamp_tp_pts(st->rr_tp * or_span, st->inst);
                sig.be_trig_pts = 0.5 * sig.tp_pts;
                sig.be_offset_pts = ir;
                sig.max_hold_bars = 180;
            } else if (st->flex_break_short && h >= st->hist.m2_ema20 && c < st->hist.m2_ema20) {
                st->flex_break_short = false;
                double sl_d = clamp_sl_pts(std::min(st->sl_base * ir * scale, or_span), st->inst);
                sig.direction = -1; sig.order_type = 0; sig.sl_pts = sl_d;
                sig.tp_pts = clamp_tp_pts(st->rr_tp * or_span, st->inst);
                sig.be_trig_pts = 0.5 * sig.tp_pts;
                sig.be_offset_pts = ir;
                sig.max_hold_bars = 180;
            }
        }
        return sig;
    }

    // Variant 3: Kasen Midpoint Retest
    if (st->variant == 3) {
        if (!st->kasen_break_long && !st->kasen_break_short) {
            if (c > oh) st->kasen_break_long = true;
            else if (c < ol) st->kasen_break_short = true;
            return sig;
        }
        double sl_d = clamp_sl_pts(st->sl_base * ir * scale, st->inst);
        if (st->kasen_break_long && l <= or_mid && c > or_mid) {
            sig.direction = 1;
        } else if (st->kasen_break_short && h >= or_mid && c < or_mid) {
            sig.direction = -1;
        }
        if (sig.direction != 0) {
            sig.order_type = 0; sig.sl_pts = sl_d;
            sig.tp_pts = (st->rr_tp > 0.0) ? clamp_tp_pts(st->rr_tp * sl_d, st->inst) : 0.0;
            sig.ratchet_trig_pts = st->r_trig_r * sl_d;
            sig.ratchet_trail_pts = std::max(st->inst.tick_size, st->r_trail_r * sl_d);
            sig.max_hold_bars = 120;
            sig.cooldown_bars = 1;
        }
        return sig;
    }

    // Variant 8: ORB Pullback (Scalper / v2 / v2.5 / Co-Opt)
    if (st->variant == 8) {
        double buf = std::clamp(0.05 * or_span + 5.0 * ir, 2.0 * ir, 40.0 * ir);
        if (c > oh + buf) { st->pb_armed_long = true; st->pb_armed_short = false; }
        else if (c < ol - buf) { st->pb_armed_short = true; st->pb_armed_long = false; }

        double tol = 10.0 * ir;
        double fib_l = (view.sess_entries == 0) ? oh : (oh - 0.382 * or_span);
        double fib_s = (view.sess_entries == 0) ? ol : (ol + 0.382 * or_span);
        double sl_d = clamp_sl_pts(st->sl_base * ir * scale, st->inst);

        if (st->pb_armed_long && l <= fib_l + tol && c > fib_l) {
            sig.direction = 1; st->pb_armed_long = false;
        } else if (st->pb_armed_short && h >= fib_s - tol && c < fib_s) {
            sig.direction = -1; st->pb_armed_short = false;
        }
        if (sig.direction != 0) {
            sig.order_type = 0; sig.sl_pts = sl_d;
            sig.tp_pts = (st->rr_tp > 0.0) ? clamp_tp_pts(st->rr_tp * sl_d, st->inst) : 0.0;
            sig.ratchet_trig_pts = st->r_trig_r * sl_d;
            sig.ratchet_trail_pts = std::max(st->inst.tick_size, st->r_trail_r * sl_d);
            sig.max_hold_bars = 90;
            sig.cooldown_bars = 1;
        }
        return sig;
    }

    // Variants 4..7: ORB v5 / v5.5 / v6 / v6.5 / Trade Slices / Eval / Payout (Resting Stop-Entry)
    double buf = st->buf_base * ir * scale;
    if (st->variant == 6 || st->variant == 7) { // 1H Pre-market ATR power-law scaling
        double ref_atr = 4.80 * ir;
        double ratio_atr = (st->hist.sess_atr_1h > 1e-6) ? (st->hist.sess_atr_1h / ref_atr) : 1.0;
        buf = std::clamp((st->buf_base * ir) * std::pow(std::max(0.05, ratio_atr), 1.25), 8.0 * ir, 90.0 * ir);
    }

    double buy_trig = oh + buf;
    double sell_trig = ol - buf;

    // Optional skip-first-trade / trade-slice gate (shadow earlier breakouts)
    if (st->min_entry_idx > 0 && st->shadow_breaks < st->min_entry_idx) {
        if (h >= buy_trig || l <= sell_trig) {
            st->shadow_breaks++;
        }
        return sig;
    }

    double sl_d = clamp_sl_pts(st->sl_base * ir * scale, st->inst);
    if (c >= buy_trig || (buy_trig - c) <= (c - sell_trig)) {
        sig.direction = 1;
        sig.trigger_px = buy_trig;
    } else {
        sig.direction = -1;
        sig.trigger_px = sell_trig;
    }
    sig.order_type = 1;
    sig.sl_pts = sl_d;
    sig.tp_pts = (st->rr_tp > 0.0) ? clamp_tp_pts(st->rr_tp * sl_d, st->inst) : 0.0;
    sig.ratchet_trig_pts = (st->r_trig_r > 0.0) ? std::max(st->inst.tick_size, st->r_trig_r * sl_d) : 0.0;
    sig.ratchet_trail_pts = (st->r_trail_r > 0.0) ? std::max(st->inst.tick_size, st->r_trail_r * sl_d) : 0.0;
    sig.max_hold_bars = 120;
    sig.cooldown_bars = 1;
    return sig;
}

// ============================================================================
// ARCHETYPE 4: Tier-1 Multiverse Suites (Dalton_IB, Lead_Lag_Mom, KER_Adaptive, London_Reset)
// Mode: 0=Dalton_IB, 1=Lead_Lag_Mom, 2=KER_Adaptive, 3=London_Reset
// ============================================================================
struct Tier1State {
    u11::InstrumentSpec inst;
    int mode = 0;
    int32_t session_mask = 0xF;
    CausalBarHistory hist;

    double buf_m = 0.35;
    double sl_base = 60.0;
    double tp_m = 1.5;
    int    param_lb = 60;
    int    max_trades = 2;
    double extra_param = 0.60; // ER threshold for KER
    int    tp_mode = 0;        // 0=Inverted_Strain_Snap (wide TP + tight trail), 1=Fixed_R, 2=Structural(mid)
};

inline void init_tier1_impl(Tier1State& st, const std::vector<double>& p, const u11::InstrumentSpec* spec,
                            int mode, int32_t mask, int tp_mode) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.mode = mode;
    st.session_mask = mask;
    st.tp_mode = tp_mode;
    st.hist = CausalBarHistory{};
    st.buf_m       = get_p(p, 0, 0.35);
    st.sl_base     = get_p(p, 1, 60.0);
    st.tp_m        = get_p(p, 2, 1.5);
    st.param_lb    = static_cast<int>(std::round(get_p(p, 3, 30.0)));
    st.max_trades  = static_cast<int>(std::round(get_p(p, 4, 2.0)));
    st.extra_param = get_p(p, 5, 0.60);
}

template <int Mode, int Mask, int TpMode>
inline void init_tier1_v(Tier1State& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_tier1_impl(st, p, s, Mode, Mask, TpMode);
}

inline u11::PluginSignal tier1_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<Tier1State*>(state_ptr);
    st->hist.sync(view, 128);

    u11::PluginSignal sig{};
    int8_t s_id = view.session_id();
    if (!sess_mask_allows(st->session_mask, s_id)) return sig;
    if (view.in_position || view.sess_entries >= st->max_trades) return sig;

    const double ir = inst_ratio(st->inst);
    const double scale = sess_index_scale(st->hist.sess_open_px, st->inst);
    const double c = static_cast<double>(view[0].c);
    const double atr = view.atr14(0);
    if (atr <= 1e-6) return sig;

    const double dyn_sl = clamp_sl_pts(st->sl_base * ir * scale, st->inst);
    int8_t dir = 0;
    double struct_tp = 0.0;

    if (st->mode == 0) { // Dalton_IB
        if (!view.or_formed(60)) return sig;
        double ib_h = view.ib60_h(), ib_l = view.ib60_l(), ib_mid = view.ib60_mid();
        if (ib_h <= ib_l) return sig;
        double dyn_buf = st->buf_m * atr;
        if (c >= ib_h - dyn_buf) {
            dir = -1;
            struct_tp = std::max(st->inst.tick_size * 4.0, c - ib_mid);
        } else if (c <= ib_l + dyn_buf) {
            dir = 1;
            struct_tp = std::max(st->inst.tick_size * 4.0, ib_mid - c);
        }
    } else if (st->mode == 1) { // Lead_Lag_Mom (Dual-SMA pullback)
        int trend_lb = std::clamp(st->param_lb, 15, 120);
        int fast_lb = (trend_lb >= 45) ? 20 : 10;
        if (st->hist.closes.count < static_cast<size_t>(trend_lb + 2)) return sig;
        double sum_tr = 0.0, sum_fa = 0.0;
        for (int i = 0; i < trend_lb; ++i) {
            double ci = st->hist.closes.at(static_cast<size_t>(i));
            sum_tr += ci;
            if (i < fast_lb) sum_fa += ci;
        }
        double sma_trend = sum_tr / static_cast<double>(trend_lb);
        double sma_fast = sum_fa / static_cast<double>(fast_lb);
        double dyn_buf = st->buf_m * atr;
        if (c > sma_trend && c <= sma_fast + dyn_buf && c >= sma_fast - 0.5 * atr) dir = 1;
        else if (c < sma_trend && c >= sma_fast - dyn_buf && c <= sma_fast + 0.5 * atr) dir = -1;
    } else if (st->mode == 2) { // KER_Adaptive
        int lb = std::clamp(st->param_lb, 5, 60);
        if (st->hist.closes.count < static_cast<size_t>(lb + 2)) return sig;
        double net_chg = std::abs(c - st->hist.closes.at(static_cast<size_t>(lb)));
        double gross = 0.0;
        double hh = -1e18, ll = 1e18;
        for (int i = 0; i < lb; ++i) {
            gross += std::abs(st->hist.closes.at(static_cast<size_t>(i)) - st->hist.closes.at(static_cast<size_t>(i + 1)));
            hh = std::max(hh, st->hist.highs.at(static_cast<size_t>(i + 1)));
            ll = std::min(ll, st->hist.lows.at(static_cast<size_t>(i + 1)));
        }
        double er = (gross > 1e-6) ? (net_chg / gross) : 0.0;
        if (er > st->extra_param) {
            double dyn_buf = st->buf_m * atr * (1.0 - er);
            if (c > hh + dyn_buf) dir = 1;
            else if (c < ll - dyn_buf) dir = -1;
        }
    } else if (st->mode == 3) { // London_Reset (prior session H/L sweep & reclaim)
        double ref_h = 0.0, ref_l = 0.0;
        bool ok = false;
        if (s_id == 2 || s_id == 3) {
            ok = st->hist.has_london; ref_h = st->hist.london_h; ref_l = st->hist.london_l;
        } else if (s_id == 1) {
            ok = st->hist.has_asian; ref_h = st->hist.asian_h; ref_l = st->hist.asian_l;
        }
        if (!ok || ref_h <= ref_l) return sig;
        double dyn_buf = st->buf_m * atr;
        double h0 = static_cast<double>(view[0].h);
        double l0 = static_cast<double>(view[0].l);
        double mid = 0.5 * (ref_h + ref_l);
        if (h0 > ref_h + dyn_buf && c < ref_h) {
            dir = -1;
            struct_tp = std::max(st->inst.tick_size * 4.0, c - mid);
        } else if (l0 < ref_l - dyn_buf && c > ref_l) {
            dir = 1;
            struct_tp = std::max(st->inst.tick_size * 4.0, mid - c);
        }
    }

    if (dir == 0) return sig;
    sig.direction = dir;
    sig.order_type = 0;
    sig.sl_pts = dyn_sl;
    if (st->tp_mode == 2 && struct_tp > 0.0) {
        sig.tp_pts = clamp_tp_pts(struct_tp, st->inst);
    } else if (st->tp_mode == 1) {
        sig.tp_pts = clamp_tp_pts(st->tp_m * dyn_sl, st->inst);
    } else {
        // Inverted_Strain_Snap: wide TP + tight 3.0*ATR interbar ratchet
        sig.tp_pts = clamp_tp_pts(st->tp_m * dyn_sl, st->inst);
        sig.ratchet_trig_pts = std::max(st->inst.tick_size, 3.0 * atr);
        sig.ratchet_trail_pts = std::max(st->inst.tick_size, 0.50 * atr);
        sig.be_trig_pts = std::max(st->inst.tick_size, 2.0 * atr);
        sig.be_offset_pts = 2.0 * ir;
    }
    sig.max_hold_bars = (st->mode == 3) ? 45 : 90;
    sig.cooldown_bars = 2;
    return sig;
}

// ============================================================================
// ARCHETYPE 5: Asian Session MR (6) + Asian Paper Matrix (9) + Asian Grid (1)
// Engine IDs:
//   0 = asian_mr_atr_compression_failed_breakout
//   1 = asian_mr_bollinger_mean_reversion
//   2 = asian_mr_opening_range_fade
//   3 = asian_mr_pivot_range_scalp
//   4 = asian_mr_rsi_extreme
//   5 = asian_mr_vwap_deviation
//   6 = asian_paper_kathy_lien_dbb
//   7 = asian_paper_rayner_teo_false_breakout
//   8 = asian_paper_tradeciety_bollinger_rsi
//   9 = asian_paper_stacey_burke_3push
//  10 = asian_paper_trading_rush_hma_macd
//  11 = asian_paper_larry_williams_pr
//  12 = asian_paper_ukspreadbetting_camarilla
//  13 = asian_paper_ezekiel_chew_pin_bar
//  14 = asian_paper_trade_prime_kdj
//  15 = asian_paper_grid_suite (multi-session DBB/BB grid)
// ============================================================================
struct AsianSuiteState {
    u11::InstrumentSpec inst;
    int engine_id = 0;
    int32_t session_mask = 0x1;
    CausalBarHistory hist;

    double sl_atr = 2.0;
    double tp_atr = 1.5;
    int    lookback = 20;
    double param_a = 2.0;
    double param_b = 1.0;
    int    max_per_sess = 1;
};

inline void init_asian_impl(AsianSuiteState& st, const std::vector<double>& p,
                            const u11::InstrumentSpec* spec, int eng_id, int32_t mask) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.engine_id = eng_id;
    st.session_mask = mask;
    st.hist = CausalBarHistory{};
    st.sl_atr       = get_p(p, 0, 2.0);
    st.tp_atr       = get_p(p, 1, 1.5);
    st.lookback     = static_cast<int>(std::round(get_p(p, 2, 20.0)));
    st.param_a      = get_p(p, 3, 2.0);
    st.param_b      = get_p(p, 4, 1.0);
    st.max_per_sess = static_cast<int>(std::round(get_p(p, 5, 1.0)));
}

template <int EngId, int Mask>
inline void init_asian_v(AsianSuiteState& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_asian_impl(st, p, s, EngId, Mask);
}

inline u11::PluginSignal asian_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<AsianSuiteState*>(state_ptr);
    st->hist.sync(view, 128);

    u11::PluginSignal sig{};
    if (!sess_mask_allows(st->session_mask, view.session_id())) return sig;
    if (view.in_position || view.sess_entries >= st->max_per_sess) return sig;

    const double atr = view.atr14(0);
    if (atr <= 1e-6 || st->hist.closes.count < 30) return sig;

    const double c = static_cast<double>(view[0].c);
    const double o = static_cast<double>(view[0].o);
    const double h = static_cast<double>(view[0].h);
    const double l = static_cast<double>(view[0].l);
    const double c1 = static_cast<double>(view[-1].c);
    const double h1 = static_cast<double>(view[-1].h);
    const double l1 = static_cast<double>(view[-1].l);

    auto bb_stats = [&](int len, size_t offset, double& mean_o, double& std_o) {
        size_t n = static_cast<size_t>(std::clamp(len, 5, 60));
        double s = 0.0, s2 = 0.0;
        for (size_t i = offset; i < offset + n; ++i) {
            double x = st->hist.closes.at(i);
            s += x; s2 += x * x;
        }
        mean_o = s / static_cast<double>(n);
        std_o = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - mean_o * mean_o));
    };

    int8_t dir = 0;
    switch (st->engine_id) {
        case 0: { // ATR compression failed breakout
            int lb = std::clamp(st->lookback, 5, 30);
            double max_atr = 0.0;
            for (size_t i = 1; i <= 14 && i < st->hist.atr14_hist.count; ++i) {
                max_atr = std::max(max_atr, st->hist.atr14_hist.at(i));
            }
            if ((h - l) < st->param_a * max_atr) {
                double hh = -1e18, ll = 1e18;
                for (int i = 1; i <= lb; ++i) {
                    hh = std::max(hh, st->hist.highs.at(static_cast<size_t>(i)));
                    ll = std::min(ll, st->hist.lows.at(static_cast<size_t>(i)));
                }
                if (h > hh && c < hh) dir = -1;
                else if (l < ll && c > ll) dir = 1;
            }
            break;
        }
        case 1: { // Bollinger MR
            double m = 0.0, sd = 0.0;
            bb_stats(st->lookback, 0, m, sd);
            double up = m + st->param_a * sd, lo = m - st->param_a * sd;
            if (l <= lo && c > lo) dir = 1;
            else if (h >= up && c < up) dir = -1;
            break;
        }
        case 2: { // Opening Range Fade (30m)
            if (view.or_formed(30)) {
                if (h >= view.or_high(30)) dir = -1;
                else if (l <= view.or_low(30)) dir = 1;
            }
            break;
        }
        case 3: { // Pivot Range Scalp (first 90m)
            if (st->hist.piv90_formed) {
                if (h >= st->hist.piv90_h) dir = -1;
                else if (l <= st->hist.piv90_l) dir = 1;
            }
            break;
        }
        case 4: { // RSI Extreme
            if (st->hist.prev_rsi14 < st->param_a && st->hist.rsi14 >= st->param_a) dir = 1;
            else if (st->hist.prev_rsi14 > st->param_b && st->hist.rsi14 <= st->param_b) dir = -1;
            break;
        }
        case 5: { // VWAP Deviation
            double vw = view.session_vwap(0);
            if (st->hist.vwap_diffs.count >= 20) {
                double sd = st->hist.vwap_diffs.stdev();
                if (sd > 1e-6) {
                    if (c < vw - st->param_a * sd) dir = 1;
                    else if (c > vw + st->param_a * sd) dir = -1;
                }
            }
            break;
        }
        case 6:
        case 15: { // Kathy Lien Double Bollinger Band (DBB)
            double m0 = 0.0, s0 = 0.0, m1 = 0.0, s1 = 0.0;
            bb_stats(st->lookback, 0, m0, s0);
            bb_stats(st->lookback, 1, m1, s1);
            double ol1 = m1 - st->param_a * s1, ou1 = m1 + st->param_a * s1;
            double il0 = m0 - st->param_b * s0, iu0 = m0 + st->param_b * s0;
            if (l1 <= ol1 && c1 > ol1 && c > il0) dir = 1;
            else if (h1 >= ou1 && c1 < ou1 && c < iu0) dir = -1;
            break;
        }
        case 7: { // Rayner Teo False Breakout
            double ah = st->hist.cur_sess_h, al = st->hist.cur_sess_l;
            if ((h1 - l1) >= st->param_a * atr) {
                if (l1 <= al && c > al && c > o) dir = 1;
                else if (h1 >= ah && c < ah && c < o) dir = -1;
            }
            break;
        }
        case 8: { // Tradeciety Bollinger 2.5 SD + RSI divergence
            double m0 = 0.0, s0 = 0.0, m1 = 0.0, s1 = 0.0;
            bb_stats(st->lookback, 0, m0, s0);
            bb_stats(st->lookback, 1, m1, s1);
            double u0 = m0 + st->param_a * s0, l0 = m0 - st->param_a * s0;
            double u1 = m1 + st->param_a * s1, l1_bb = m1 - st->param_a * s1;
            double min_c = 1e18, max_c = -1e18, min_r = 1e18, max_r = -1e18;
            for (size_t i = 1; i <= 10 && i < st->hist.rsi14_hist.count; ++i) {
                min_c = std::min(min_c, st->hist.closes.at(i));
                max_c = std::max(max_c, st->hist.closes.at(i));
                min_r = std::min(min_r, st->hist.rsi14_hist.at(i));
                max_r = std::max(max_r, st->hist.rsi14_hist.at(i));
            }
            if (c1 < l1_bb && c > l0 && c < u0 && c < min_c && st->hist.rsi14 > min_r) dir = 1;
            else if (c1 > u1 && c < u0 && c > l0 && c > max_c && st->hist.rsi14 < max_r) dir = -1;
            break;
        }
        case 9: { // Stacey Burke 3-Push Exhaustion inside tight range
            if (st->hist.highs.count >= 20) {
                int peaks = 0, troughs = 0;
                double last_pv = 1e18, last_tv = 1e18;
                bool dec_p = false, dec_t = false;
                for (size_t i = 2; i <= 18; ++i) {
                    if (st->hist.highs.at(i) > st->hist.highs.at(i - 1) && st->hist.highs.at(i) > st->hist.highs.at(i + 1)) {
                        ++peaks;
                        dec_p = (st->hist.vols.at(i) < last_pv);
                        last_pv = st->hist.vols.at(i);
                    }
                    if (st->hist.lows.at(i) < st->hist.lows.at(i - 1) && st->hist.lows.at(i) < st->hist.lows.at(i + 1)) {
                        ++troughs;
                        dec_t = (st->hist.vols.at(i) < last_tv);
                        last_tv = st->hist.vols.at(i);
                    }
                }
                if (peaks >= 3 && dec_p && c < o && c < c1) dir = -1;
                else if (troughs >= 3 && dec_t && c > o && c > c1) dir = 1;
            }
            break;
        }
        case 10: { // Trading Rush HMA55 + MACD
            if (st->hist.closes.count >= 65) {
                bool macd_cross_up = (st->hist.prev_macd_line <= st->hist.prev_macd_sig) && (st->hist.macd_line > st->hist.macd_sig9);
                bool macd_cross_dn = (st->hist.prev_macd_line >= st->hist.prev_macd_sig) && (st->hist.macd_line < st->hist.macd_sig9);
                if (c > st->hist.hma55 && c1 <= st->hist.prev_hma55 && st->hist.macd_hist > 0 && st->hist.prev_macd_hist <= 0 && macd_cross_up && st->hist.macd_line < 0.0) dir = 1;
                else if (c < st->hist.hma55 && c1 >= st->hist.prev_hma55 && st->hist.macd_hist < 0 && st->hist.prev_macd_hist >= 0 && macd_cross_dn && st->hist.macd_line > 0.0) dir = -1;
            }
            break;
        }
        case 11: { // Larry Williams %R
            int len = std::clamp(st->lookback, 5, 30);
            if (st->hist.highs.count > static_cast<size_t>(len + 1)) {
                auto calc_pr = [&](size_t off) {
                    double hh = -1e18, ll = 1e18;
                    for (int i = 0; i < len; ++i) {
                        hh = std::max(hh, st->hist.highs.at(off + static_cast<size_t>(i)));
                        ll = std::min(ll, st->hist.lows.at(off + static_cast<size_t>(i)));
                    }
                    double xc = st->hist.closes.at(off);
                    return (hh > ll) ? (-100.0 * (hh - xc) / (hh - ll)) : -50.0;
                };
                double pr0 = calc_pr(0), pr1 = calc_pr(1);
                if (pr1 < -st->param_a && pr0 > -st->param_b) dir = 1;
                else if (pr1 > -(100.0 - st->param_a) && pr0 < -(100.0 - st->param_b)) dir = -1;
            }
            break;
        }
        case 12: { // UKSpreadbetting Camarilla S3/R3
            if (view.has_prior_day()) {
                double pdh = view.pdh(), pdl = view.pdl(), pdc = view.pdc();
                double rng = pdh - pdl;
                if (rng > 0.0) {
                    double r3 = pdc + rng * 1.1 / 4.0;
                    double s3 = pdc - rng * 1.1 / 4.0;
                    if (h1 >= r3 && c < o) dir = -1;
                    else if (l1 <= s3 && c > o) dir = 1;
                }
            }
            break;
        }
        case 13: { // Ezekiel Chew Pin Bar
            double rng_60 = 0.0, hh60 = -1e18, ll60 = 1e18;
            size_t n_lb = std::min<size_t>(60, st->hist.highs.count);
            for (size_t i = 1; i < n_lb; ++i) {
                hh60 = std::max(hh60, st->hist.highs.at(i));
                ll60 = std::min(ll60, st->hist.lows.at(i));
            }
            rng_60 = hh60 - ll60;
            double body = std::abs(c - o);
            double lwick = std::min(o, c) - l;
            double uwick = h - std::max(o, c);
            if (rng_60 > 0.0 && body > 0.0) {
                if (lwick >= 2.0 * body && c > o && c < ll60 + 0.25 * rng_60) dir = 1;
                else if (uwick >= 2.0 * body && c < o && c > ll60 + 0.75 * rng_60) dir = -1;
            }
            break;
        }
        case 14: { // Trade Prime KDJ
            double chop_w = st->param_a * st->inst.tick_size;
            double hh = -1e18, ll = 1e18;
            for (size_t i = 1; i <= 30 && i < st->hist.highs.count; ++i) {
                hh = std::max(hh, st->hist.highs.at(i));
                ll = std::min(ll, st->hist.lows.at(i));
            }
            if ((hh - ll) <= chop_w) {
                if (st->hist.prev_kdj_j < 0.0 && st->hist.kdj_j > 0.0 && st->hist.kdj_j >= st->hist.kdj_k && st->hist.prev_kdj_j < st->hist.prev_kdj_k) dir = 1;
                else if (st->hist.prev_kdj_j > 100.0 && st->hist.kdj_j < 100.0 && st->hist.kdj_j <= st->hist.kdj_k && st->hist.prev_kdj_j > st->hist.prev_kdj_k) dir = -1;
            }
            break;
        }
    }

    if (dir == 0) return sig;
    sig.direction = dir;
    sig.order_type = 0;
    sig.sl_pts = clamp_sl_pts(st->sl_atr * atr, st->inst);
    sig.tp_pts = clamp_tp_pts(st->tp_atr * atr, st->inst);
    if (st->engine_id == 15) {
        sig.ratchet_trig_pts = std::max(st->inst.tick_size, 0.50 * atr);
        sig.ratchet_trail_pts = std::max(st->inst.tick_size, 0.25 * atr);
    }
    sig.max_hold_bars = 90;
    sig.cooldown_bars = 2;
    return sig;
}

// ============================================================================
// ARCHETYPE 6: Paper 1 (7 Engines) + Paper 2 (9 Engines)
// Engine IDs:
//   0 = p1_ict_silver_bullet
//   1 = p1_casper_inverted_fvg
//   2 = p1_velez_20_200_elephant_bar
//   3 = p1_rosato_sd_absorption
//   4 = p1_carter_ttm_squeeze
//   5 = p1_raschke_holy_grail
//   6 = p1_wade_pats_second_entry
//   7 = p2_liquidity_purge_2022
//   8 = p2_structural_confluence_unicorn
//   9 = p2_smt_divergence (single-asset Price/RSI divergence per paper2_matrix.py)
//  10 = p2_tick_price_action_2000
//  11 = p2_vpoc_reversion
//  12 = p2_level3_footprint_absorption
//  13 = p2_atr_fair_value_scalp
//  14 = p2_vwap_zscore_orb
//  15 = p2_algorithmic_indicator_convergence
// ============================================================================
struct PaperSuiteState {
    u11::InstrumentSpec inst;
    int engine_id = 0;
    int32_t session_mask = 0xF;
    CausalBarHistory hist;

    double sl_atr = 3.0;
    double tp_atr = 2.0;
    int    lookback = 20;
    double param_a = 1.5;
    double param_b = 25.0;
    int    max_per_day = 1;

    // Stateful trackers for Silver Bullet / Inverted FVG / PATs / SMT
    bool   day_has_bull_fvg = false, day_has_bear_fvg = false;
    bool   has_inv_bull_fvg = false, has_inv_bear_fvg = false;
    double inv_bull_bot = 0.0, inv_bull_top = 0.0, inv_bear_bot = 0.0, inv_bear_top = 0.0;
    double last_sh = 0.0, last_sl = 0.0;
    bool   has_sh = false, has_sl = false;
    int    sh_failures = 0, sl_failures = 0;
    double last_hi_px = 0.0, last_hi_rsi = 0.0, last_lo_px = 0.0, last_lo_rsi = 0.0;
    bool   has_hi_div = false, has_lo_div = false;
    bool   prev_squeezed = false;
    int32_t cur_day = -1;
    int     trades_today = 0;
};

inline void init_paper_impl(PaperSuiteState& st, const std::vector<double>& p,
                            const u11::InstrumentSpec* spec, int eng_id) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.engine_id = eng_id;
    st.hist = CausalBarHistory{};
    st.hist.track_heavy_vp = (eng_id == 11);
    st.sl_atr      = get_p(p, 0, (eng_id < 7) ? 5.0 : 3.0);
    st.tp_atr      = get_p(p, 1, 2.0);
    st.lookback    = static_cast<int>(std::round(get_p(p, 2, 20.0)));
    st.param_a     = get_p(p, 3, 1.5);
    st.param_b     = get_p(p, 4, 25.0);
    st.max_per_day = static_cast<int>(std::round(get_p(p, 5, 1.0)));
    st.session_mask = 0xF;
}

template <int EngId>
inline void init_paper_v(PaperSuiteState& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_paper_impl(st, p, s, EngId);
}

inline u11::PluginSignal paper_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<PaperSuiteState*>(state_ptr);
    st->hist.sync(view, 256);

    u11::PluginSignal sig{};
    if (view.cme_day_idx() != st->cur_day) {
        st->cur_day = view.cme_day_idx();
        st->trades_today = 0;
        st->day_has_bull_fvg = st->day_has_bear_fvg = false;
    }

    const double atr = view.atr14(0);
    if (atr <= 1e-6 || st->hist.closes.count < 30) return sig;

    const double c = static_cast<double>(view[0].c);
    const double o = static_cast<double>(view[0].o);
    const double h = static_cast<double>(view[0].h);
    const double l = static_cast<double>(view[0].l);
    const double rv = view.rel_vol(0);

    // Causal 3-bar FVG & Swing updates on current bar
    double l0 = static_cast<double>(view[0].l), h0 = static_cast<double>(view[0].h);
    double h2 = static_cast<double>(view[-2].h), l2 = static_cast<double>(view[-2].l);
    if (l0 > h2) {
        st->day_has_bull_fvg = true;
        st->has_inv_bull_fvg = true; st->inv_bull_bot = h2; st->inv_bull_top = l0;
    }
    if (h0 < l2) {
        st->day_has_bear_fvg = true;
        st->has_inv_bear_fvg = true; st->inv_bear_bot = h0; st->inv_bear_top = l2;
    }

    // Causal local swing check (confirmed 1 bar after peak)
    if (st->hist.highs.count >= 10) {
        double h1 = st->hist.highs.at(1), l1 = st->hist.lows.at(1);
        double max_oth = -1e18, min_oth = 1e18;
        for (size_t i = 2; i <= 6; ++i) {
            max_oth = std::max(max_oth, st->hist.highs.at(i));
            min_oth = std::min(min_oth, st->hist.lows.at(i));
        }
        if (h1 >= max_oth && h1 > h0) { st->last_sh = h1; st->has_sh = true; st->sh_failures = 0; }
        if (l1 <= min_oth && l1 < l0) { st->last_sl = l1; st->has_sl = true; st->sl_failures = 0; }
    }

    if (view.in_position || !sess_mask_allows(st->session_mask, view.session_id())) return sig;
    if (st->trades_today >= st->max_per_day) return sig;

    int8_t dir = 0;
    int8_t ord_type = 0;
    double trig_px = 0.0;

    switch (st->engine_id) {
        case 0: { // P1_01 ICT Silver Bullet
            if (st->hist.has_prev_m15) {
                double mid = 0.5 * (st->hist.prev_m15_h + st->hist.prev_m15_l);
                if (l <= st->hist.prev_m15_l && c > st->hist.prev_m15_l && c >= mid && st->day_has_bull_fvg) dir = 1;
                else if (h >= st->hist.prev_m15_h && c < st->hist.prev_m15_h && c <= mid && st->day_has_bear_fvg) dir = -1;
            }
            break;
        }
        case 1: { // P1_02 Casper Inverted FVG
            if (st->has_inv_bull_fvg && l <= st->inv_bull_bot && c < st->inv_bull_bot) dir = -1;
            else if (st->has_inv_bear_fvg && h >= st->inv_bear_top && c > st->inv_bear_top) dir = 1;
            break;
        }
        case 2: { // P1_03 Velez 20/200 Elephant Bar
            if (st->hist.closes.count >= 200) {
                double sma200 = 0.0;
                for (size_t i = 0; i < 200; ++i) sma200 += st->hist.closes.at(i);
                sma200 /= 200.0;
                double rng = h - l;
                if (rng >= st->param_a * atr && rng > 0.0) {
                    if ((h - c) <= 0.2 * rng && c > st->hist.ema20 && c > sma200) dir = 1;
                    else if ((c - l) <= 0.2 * rng && c < st->hist.ema20 && c < sma200) dir = -1;
                }
            }
            break;
        }
        case 3: { // P1_04 Rosato S/D Absorption
            if (st->hist.has_prev_m15 && st->hist.vols.count >= 5) {
                bool vol_absorb = (st->hist.vols.at(1) < st->hist.vols.mean());
                if (l <= st->hist.prev_m15_l && vol_absorb && c > o && c > st->hist.ema20) dir = 1;
                else if (h >= st->hist.prev_m15_h && vol_absorb && c < o && c < st->hist.ema20) dir = -1;
            }
            break;
        }
        case 4: { // P1_05 Carter TTM Squeeze
            int n = std::clamp(st->lookback, 10, 40);
            double s = 0.0, s2 = 0.0;
            for (int i = 0; i < n; ++i) {
                double x = st->hist.closes.at(static_cast<size_t>(i));
                s += x; s2 += x * x;
            }
            double m = s / static_cast<double>(n);
            double sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - m * m));
            bool sq_now = (2.0 * sd <= st->param_a * atr);
            if (st->prev_squeezed && !sq_now && st->hist.closes.count > 12) {
                double mom = st->hist.closes.at(1) - st->hist.closes.at(12);
                if (mom > 0.0) dir = 1;
                else if (mom < 0.0) dir = -1;
            }
            st->prev_squeezed = sq_now;
            break;
        }
        case 5: { // P1_06 Linda Raschke Holy Grail
            if (st->hist.closes.count >= 25 && (h - l) > 0.0) {
                double ema_prev3 = 0.0;
                for (size_t i = 3; i < 23; ++i) ema_prev3 += st->hist.closes.at(i);
                ema_prev3 /= 20.0;
                double slope = st->hist.ema20 - ema_prev3;
                if (st->hist.adx14 >= st->param_b) {
                    if (slope > 0.0 && (st->hist.ema20 - l) >= st->param_a * (h - l) && c > st->hist.ema20) {
                        dir = 1; ord_type = 1; trig_px = h + st->inst.tick_size;
                    } else if (slope < 0.0 && (h - st->hist.ema20) >= st->param_a * (h - l) && c < st->hist.ema20) {
                        dir = -1; ord_type = 1; trig_px = l - st->inst.tick_size;
                    }
                }
            }
            break;
        }
        case 6: { // P1_07 Wade PATs Second Entry
            if (st->has_sh) {
                if (h > st->last_sh && c < st->last_sh) st->sh_failures++;
                if (st->sh_failures >= 1 && c > st->last_sh) {
                    dir = 1; ord_type = 1; trig_px = h + st->inst.tick_size;
                }
            }
            if (dir == 0 && st->has_sl) {
                if (l < st->last_sl && c > st->last_sl) st->sl_failures++;
                if (st->sl_failures >= 1 && c < st->last_sl) {
                    dir = -1; ord_type = 1; trig_px = l - st->inst.tick_size;
                }
            }
            break;
        }
        case 7: { // P2_01 2022 ICT Liquidity Purge
            if (st->has_sl && l <= st->last_sl && c > st->last_sl && c <= st->last_sl + st->param_a * atr) dir = 1;
            else if (st->has_sh && h >= st->last_sh && c < st->last_sh && c >= st->last_sh - st->param_a * atr) dir = -1;
            break;
        }
        case 8: { // P2_02 Structural Confluence Unicorn
            double vw = view.session_vwap(0);
            double rng = h - l;
            if (rng > 0.0 && rv > st->param_a && st->hist.adx14 >= st->param_b) {
                if (c > st->hist.ema9 && st->hist.ema9 > st->hist.ema21 && c > vw && (h - c) <= 0.2 * rng) dir = 1;
                else if (c < st->hist.ema9 && st->hist.ema9 < st->hist.ema21 && c < vw && (c - l) <= 0.2 * rng) dir = -1;
            }
            break;
        }
        case 9: { // P2_03 SMT Price/RSI Divergence
            if (st->has_sh && (!st->has_hi_div || st->last_sh > st->last_hi_px)) {
                st->last_hi_px = st->last_sh; st->last_hi_rsi = st->hist.rsi14; st->has_hi_div = true;
            }
            if (st->has_sl && (!st->has_lo_div || st->last_sl < st->last_lo_px)) {
                st->last_lo_px = st->last_sl; st->last_lo_rsi = st->hist.rsi14; st->has_lo_div = true;
            }
            if (st->has_lo_div && l <= st->last_lo_px && st->hist.rsi14 > st->last_lo_rsi && c > l) dir = 1;
            else if (st->has_hi_div && h >= st->last_hi_px && st->hist.rsi14 < st->last_hi_rsi && c < h) dir = -1;
            break;
        }
        case 10: { // P2_04 2000-Tick Price Action Master Setup
            if (st->hist.ranges.count >= 6) {
                double max_r = 0.0, wh = -1e18, wl = 1e18;
                for (size_t i = 1; i <= 5; ++i) {
                    max_r = std::max(max_r, st->hist.ranges.at(i));
                    wh = std::max(wh, st->hist.highs.at(i));
                    wl = std::min(wl, st->hist.lows.at(i));
                }
                if (max_r <= st->param_a * atr && (h - l) >= atr) {
                    if (c > wh) dir = 1;
                    else if (c < wl) dir = -1;
                }
            }
            break;
        }
        case 11: { // P2_05 VPOC Mean Reversion
            if (st->hist.has_pd_vp) {
                if (c < st->hist.pd_poc - st->param_a * atr && c > o) dir = 1;
                else if (c > st->hist.pd_poc + st->param_a * atr && c < o) dir = -1;
            } else {
                double vw = view.session_vwap(0);
                if (c < vw - st->param_a * atr && c > o) dir = 1;
                else if (c > vw + st->param_a * atr && c < o) dir = -1;
            }
            break;
        }
        case 12: { // P2_06 Level III Footprint Absorption
            if ((h - l) <= st->param_a * atr && rv >= 1.8) {
                double ll6 = 1e18, hh6 = -1e18;
                for (size_t i = 0; i < 6; ++i) {
                    ll6 = std::min(ll6, st->hist.lows.at(i));
                    hh6 = std::max(hh6, st->hist.highs.at(i));
                }
                if (l <= ll6 && c > o) dir = 1;
                else if (h >= hh6 && c < o) dir = -1;
            }
            break;
        }
        case 13: { // P2_07 ATR Fair Value Scalp
            double fv = view.session_vwap(0);
            if (c < fv - st->param_a * atr && c > o && st->hist.rsi14 > 30.0) dir = 1;
            else if (c > fv + st->param_a * atr && c < o && st->hist.rsi14 < 70.0) dir = -1;
            break;
        }
        case 14: { // P2_08 VWAP Z-Score ORB
            if (view.or_formed(15)) {
                double vw = view.session_vwap(0);
                double vsd = std::max(0.25 * atr, view.session_vwap_std(0));
                double z = (c - vw) / vsd;
                if (c > view.or_high(15) && z >= st->param_a) dir = 1;
                else if (c < view.or_low(15) && z <= -st->param_a) dir = -1;
            }
            break;
        }
        case 15: { // P2_09 Algorithmic Indicator Convergence
            double vw = view.session_vwap(0);
            if (st->hist.adx14 >= st->param_b && rv > st->param_a) {
                if (st->hist.rsi14 > 50.0 && st->hist.macd_hist > 0.0 && c > vw) dir = 1;
                else if (st->hist.rsi14 < 50.0 && st->hist.macd_hist < 0.0 && c < vw) dir = -1;
            }
            break;
        }
    }

    if (dir == 0) return sig;
    st->trades_today++;
    sig.direction = dir;
    sig.order_type = ord_type;
    sig.trigger_px = trig_px;
    sig.sl_pts = clamp_sl_pts(st->sl_atr * atr, st->inst);
    sig.tp_pts = clamp_tp_pts(st->tp_atr * atr, st->inst);
    sig.max_hold_bars = 90;
    sig.cooldown_bars = 2;
    return sig;
}

// ============================================================================
// ARCHETYPE 7: Topstep Quant Suite 12 & Standalones (21 UIDs)
// Engine IDs:
//   0 = quant_amt_value
//   1 = quant_bb_kc_squeeze
//   2 = quant_beta_leadlag (needs_cross_asset = true)
//   3 = quant_climax_snapback
//   4 = quant_ker_regime
//   5 = quant_kinetic_velocity
//   6 = quant_ldn_ny_reset
//   7 = quant_moc_drift
//   8 = quant_smt_divergence (needs_cross_asset = true)
//   9 = quant_turtle_soup
//  10 = quant_vsa_absorption
//  11 = quant_vwap_multi_anchor
//  12 = bar_color_flip
//  13 = bollinger_bands
//  14 = ema_cross_scalp
//  15 = nitro_crt (causal clean-room)
//  16 = session_switch (London->NY resting limit order_type=2)
//  17 = vwap_fade
//  18 = reel_27_model_d_pdh_pdl
//  19 = qullamaggie_trading_system
//  20 = fvg_execution_engine
// ============================================================================
struct QuantStandaloneState {
    u11::InstrumentSpec inst;
    int engine_id = 0;
    int32_t session_mask = 0xF;
    CausalBarHistory hist;

    double param0 = 1.0;
    double param1 = 2.0;
    int    lookback = 20;
    double sl_base = 15.0;
    double tp_base = 30.0;
    int    max_per_day = 4;

    int32_t cur_day = -1;
    int     trades_today = 0;
    int     above_vah_cnt = 0;
    bool    prev_squeezed = false;
    bool    has_zone = false;
    int8_t  zone_dir = 0;
    double  zone_lo = 0.0, zone_hi = 0.0;
    bool    sw_swept_hi = false, sw_swept_lo = false;
};

inline void init_quant_impl(QuantStandaloneState& st, const std::vector<double>& p,
                            const u11::InstrumentSpec* spec, int eng_id, int32_t mask) {
    st.inst = spec ? *spec : u11::get_instrument_spec("NQ");
    st.engine_id = eng_id;
    st.session_mask = mask;
    st.hist = CausalBarHistory{};
    st.hist.track_heavy_vp = (eng_id == 0);
    st.param0      = get_p(p, 0, 1.0);
    st.param1      = get_p(p, 1, 2.0);
    st.lookback    = static_cast<int>(std::round(get_p(p, 2, 20.0)));
    st.sl_base     = get_p(p, 3, 15.0);
    st.tp_base     = get_p(p, 4, 30.0);
    st.max_per_day = static_cast<int>(std::round(get_p(p, 5, 4.0)));
}

template <int EngId, int Mask>
inline void init_quant_v(QuantStandaloneState& st, const std::vector<double>& p, const u11::InstrumentSpec* s) {
    init_quant_impl(st, p, s, EngId, Mask);
}

inline u11::PluginSignal quant_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    auto* st = static_cast<QuantStandaloneState*>(state_ptr);
    st->hist.sync(view, 128);

    u11::PluginSignal sig{};
    if (view.cme_day_idx() != st->cur_day) {
        st->cur_day = view.cme_day_idx();
        st->trades_today = 0;
        st->above_vah_cnt = 0;
        st->has_zone = false;
        st->sw_swept_hi = st->sw_swept_lo = false;
    }

    // Session Switch: detect London 5m indecision+impulse zone during 02:00-08:30 ET (including causal lookback if NY-only session mask skipped London)
    const int16_t em = view.et_mins();
    if (st->engine_id == 16 && !st->has_zone && em >= 570 && em < 620 && view.bar_index() >= 300) {
        int32_t today = view.day_yyyymmdd();
        for (int k = 1; k <= 500 && static_cast<size_t>(k) <= view.bar_index(); k += 5) {
            int16_t em_k = 0; int32_t dt_k = 0;
            u11::decode_et_and_cme_date(view[-k].ts_sec, em_k, dt_k);
            if (dt_k != today) break;
            if (em_k >= 180 && em_k < 350 && static_cast<size_t>(k + 15) <= view.bar_index()) {
                double ch = -1e18, cl = 1e18, co = static_cast<double>(view[-(k + 14)].o), cc = static_cast<double>(view[-(k + 10)].c);
                for (int m = k + 10; m <= k + 14; ++m) { ch = std::max(ch, static_cast<double>(view[-m].h)); cl = std::min(cl, static_cast<double>(view[-m].l)); }
                double fc = static_cast<double>(view[-k].c);
                double rng = ch - cl, avg_r = std::max(st->inst.tick_size, 4.0 * view.atr14(-k));
                if (rng > 0.0 && std::abs(cc - co) <= 0.45 * rng && rng >= 0.3 * avg_r) {
                    if ((fc - cc) >= 1.2 * avg_r && fc > ch) { st->has_zone = true; st->zone_dir = 1; st->zone_lo = cl; st->zone_hi = ch; break; }
                    if ((cc - fc) >= 1.2 * avg_r && fc < cl) { st->has_zone = true; st->zone_dir = -1; st->zone_lo = cl; st->zone_hi = ch; break; }
                }
            }
        }
    }
    if (st->engine_id == 16 && st->hist.m5_just_closed && em >= 120 && em < 510 && st->hist.m5_c_w.count >= 12) {
        double co = st->hist.m5_o_w.at(2), ch = st->hist.m5_h_w.at(2), cl = st->hist.m5_l_w.at(2), cc = st->hist.m5_c_w.at(2);
        double f_c = st->hist.m5_c_w.at(0), f_o = st->hist.m5_o_w.at(1), f_c1 = st->hist.m5_c_w.at(1);
        double rng = ch - cl;
        double avg_r = std::max(st->inst.tick_size, st->hist.m5_atr14);
        if (rng > 0.0 && std::abs(cc - co) <= 0.35 * rng && rng >= 0.4 * avg_r && rng <= 2.5 * avg_r) {
            if ((f_c - cc) >= 1.5 * avg_r && f_c > ch && f_c1 > f_o) {
                st->has_zone = true; st->zone_dir = 1; st->zone_lo = cl; st->zone_hi = ch;
            } else if ((cc - f_c) >= 1.5 * avg_r && f_c < cl && f_c1 < f_o) {
                st->has_zone = true; st->zone_dir = -1; st->zone_lo = cl; st->zone_hi = ch;
            }
        }
    }

    if (view.in_position || !sess_mask_allows(st->session_mask, view.session_id())) return sig;
    if (st->trades_today >= st->max_per_day) return sig;

    const double atr = view.atr14(0);
    if (atr <= 1e-6 || st->hist.closes.count < 25) return sig;

    const double ir = inst_ratio(st->inst);
    const double c = static_cast<double>(view[0].c);
    const double o = static_cast<double>(view[0].o);
    const double h = static_cast<double>(view[0].h);
    const double l = static_cast<double>(view[0].l);
    const double rv = view.rel_vol(0);

    int8_t dir = 0;
    int8_t ord_type = 0;
    double trig_px = 0.0;
    double sl_d = clamp_sl_pts(st->sl_base * atr, st->inst);
    double tp_d = clamp_tp_pts(st->tp_base * atr, st->inst);
    int max_hold = 60;

    switch (st->engine_id) {
        case 0: { // amt_value
            if (!st->hist.has_pd_vp) return sig;
            if (h > st->hist.pd_vah && c < st->hist.pd_vah && c > st->hist.pd_poc) {
                dir = -1; sl_d = clamp_sl_pts(st->sl_base * atr, st->inst);
                tp_d = clamp_tp_pts(c - st->hist.pd_poc, st->inst);
            } else if (l < st->hist.pd_val && c > st->hist.pd_val && c < st->hist.pd_poc) {
                dir = 1; sl_d = clamp_sl_pts(st->sl_base * atr, st->inst);
                tp_d = clamp_tp_pts(st->hist.pd_poc - c, st->inst);
            } else if (c > st->hist.pd_vah && rv > st->param0) {
                st->above_vah_cnt++;
                if (st->above_vah_cnt >= 2) {
                    dir = 1; st->above_vah_cnt = 0;
                    sl_d = clamp_sl_pts(st->sl_base * atr, st->inst);
                    tp_d = clamp_tp_pts(st->tp_base * atr, st->inst);
                }
            } else {
                st->above_vah_cnt = 0;
            }
            max_hold = 120;
            break;
        }
        case 1: { // bb_kc_squeeze
            int n = std::clamp(st->lookback, 10, 40);
            double s = 0.0, s2 = 0.0;
            for (int i = 0; i < n; ++i) {
                double x = st->hist.closes.at(static_cast<size_t>(i));
                s += x; s2 += x * x;
            }
            double m = s / static_cast<double>(n);
            double sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - m * m));
            double bbu = m + st->param0 * sd, bbl = m - st->param0 * sd;
            double kcu = st->hist.ema20 + st->param1 * atr, kcl = st->hist.ema20 - st->param1 * atr;
            bool sq_now = (bbu < kcu && bbl > kcl);
            if (st->prev_squeezed) {
                double mom = c - st->hist.ema20;
                if (bbu > kcu && mom > 0.0) dir = 1;
                else if (bbl < kcl && mom < 0.0) dir = -1;
            }
            st->prev_squeezed = sq_now;
            break;
        }
        case 2: { // beta_leadlag (Cross-Asset Causal!)
            const bool is_nq = (st->inst.baseline_index > 4000.0 && st->inst.baseline_index < 6000.0);
            bool has_lead = is_nq ? (view.has_ref_es() || view.has_ref_ym())
                                  : (view.has_ref_nq() || view.has_ref_es() || view.has_ref_ym());
            if (!has_lead) return sig;
            int lb = std::clamp(st->lookback, 5, 30);
            double self_hh = -1e18, lead_hh = -1e18;
            double lead_c = 0.0, lead_atr = 1.0;
            for (int i = 1; i <= lb; ++i) {
                self_hh = std::max(self_hh, static_cast<double>(view[-i].h));
            }
            if (is_nq && view.has_ref_es()) {
                lead_c = static_cast<double>(view.ref_es_bar(0).c);
                lead_atr = view.ref_es_atr14(0);
                for (int i = 1; i <= lb; ++i) lead_hh = std::max(lead_hh, static_cast<double>(view.ref_es_bar(-i).h));
            } else if (view.has_ref_ym()) {
                lead_c = static_cast<double>(view.ref_ym_bar(0).c);
                lead_atr = view.ref_ym_atr14(0);
                for (int i = 1; i <= lb; ++i) lead_hh = std::max(lead_hh, static_cast<double>(view.ref_ym_bar(-i).h));
            } else if (view.has_ref_nq()) {
                lead_c = static_cast<double>(view.ref_nq_bar(0).c);
                lead_atr = view.ref_nq_atr14(0);
                for (int i = 1; i <= lb; ++i) lead_hh = std::max(lead_hh, static_cast<double>(view.ref_nq_bar(-i).h));
            }
            if (lead_atr > 1e-6) {
                double lead_up = (lead_c - lead_hh) / lead_atr;
                double lag_up  = (c - self_hh) / atr;
                if (lead_up > st->param0 && lag_up <= st->param1) dir = 1;
            }
            max_hold = 30;
            break;
        }
        case 3: { // climax_snapback (5m derived)
            if (st->hist.m5_just_closed && st->hist.m5_c_w.count >= 5 && st->hist.m5_atr14 > 1e-6) {
                double c1 = st->hist.m5_c_w.at(1), c4 = st->hist.m5_c_w.at(4);
                double disp = std::abs(c1 - c4);
                bool exp_rng = (st->hist.m5_rng_w.at(1) > st->hist.m5_rng_w.at(2)) && (st->hist.m5_rng_w.at(2) > st->hist.m5_rng_w.at(3));
                bool exh_vol = (st->hist.m5_v_w.at(1) < st->hist.m5_v_w.at(2));
                if (disp > st->param0 * st->hist.m5_atr14 && exp_rng && exh_vol) {
                    double c0 = st->hist.m5_c_w.at(0);
                    double mid = 0.5 * (c1 + c4);
                    if (c1 > c4 && c0 < c1 && c0 > mid) {
                        dir = -1; sl_d = clamp_sl_pts(st->hist.m5_h_w.at(1) - c0 + st->inst.tick_size, st->inst);
                        tp_d = clamp_tp_pts(c0 - mid, st->inst);
                    } else if (c1 < c4 && c0 > c1 && c0 < mid) {
                        dir = 1; sl_d = clamp_sl_pts(c0 - st->hist.m5_l_w.at(1) + st->inst.tick_size, st->inst);
                        tp_d = clamp_tp_pts(mid - c0, st->inst);
                    }
                }
            }
            max_hold = 20;
            break;
        }
        case 4: { // ker_regime
            int lb = std::clamp(st->lookback, 10, 40);
            if (st->hist.closes.count > static_cast<size_t>(lb + 2)) {
                double net = std::abs(c - st->hist.closes.at(static_cast<size_t>(lb)));
                double gross = 0.0, hh = -1e18, ll = 1e18;
                for (int i = 0; i < lb; ++i) {
                    gross += std::abs(st->hist.closes.at(static_cast<size_t>(i)) - st->hist.closes.at(static_cast<size_t>(i + 1)));
                    hh = std::max(hh, st->hist.highs.at(static_cast<size_t>(i + 1)));
                    ll = std::min(ll, st->hist.lows.at(static_cast<size_t>(i + 1)));
                }
                double er = (gross > 1e-6) ? (net / gross) : 0.0;
                double mid = 0.5 * (hh + ll);
                if (er > st->param0) {
                    if (c > hh) dir = 1; else if (c < ll) dir = -1;
                } else if (er < st->param1) {
                    if (c >= hh && c > mid) { dir = -1; sl_d = clamp_sl_pts(h - c + 0.25 * atr, st->inst); tp_d = clamp_tp_pts(c - mid, st->inst); }
                    else if (c <= ll && c < mid) { dir = 1; sl_d = clamp_sl_pts(c - l + 0.25 * atr, st->inst); tp_d = clamp_tp_pts(mid - c, st->inst); }
                }
            }
            max_hold = 40;
            break;
        }
        case 5: { // kinetic_velocity
            if (st->hist.vel_hist.count >= 50 && rv > st->param1) {
                double m = st->hist.vel_hist.mean(), sd = st->hist.vel_hist.stdev();
                if (sd > 1e-9) {
                    double v0 = st->hist.vel_hist.at(0), v1 = st->hist.vel_hist.at(1);
                    double z = (v0 - m) / sd;
                    if (std::abs(z) > st->param0 && std::abs(v0 - v1) > 0.0) {
                        dir = (z > 0.0) ? 1 : -1;
                        double dpx = std::abs(v0) * 5.0;
                        sl_d = clamp_sl_pts(0.5 * dpx, st->inst);
                        tp_d = clamp_tp_pts(1.5 * dpx, st->inst);
                    }
                }
            }
            max_hold = 3;
            break;
        }
        case 6: { // ldn_ny_reset (08:30-09:30 / NY open sweep of London H/L)
            if (st->hist.has_london && em >= 510 && em < 630) {
                double lh = st->hist.london_h, ll = st->hist.london_l;
                double mid = 0.5 * (lh + ll);
                double up = h - lh, dn = ll - l;
                if (!st->sw_swept_hi && up >= st->param0 * atr && up <= st->param1 * atr && c < lh && c > mid) {
                    dir = -1; st->sw_swept_hi = true;
                    sl_d = clamp_sl_pts(h - c + st->sl_base * atr, st->inst);
                    tp_d = clamp_tp_pts(c - mid, st->inst);
                } else if (!st->sw_swept_lo && dn >= st->param0 * atr && dn <= st->param1 * atr && c > ll && c < mid) {
                    dir = 1; st->sw_swept_lo = true;
                    sl_d = clamp_sl_pts(c - l + st->sl_base * atr, st->inst);
                    tp_d = clamp_tp_pts(mid - c, st->inst);
                }
            }
            max_hold = 120;
            break;
        }
        case 7: { // moc_drift (15:15 ET momentum vs 15:00 anchor + VWAP)
            if (em == 915 && st->hist.has_c_1500) {
                double mom = c - st->hist.c_1500;
                double vw = view.session_vwap(0);
                if (mom > st->param0 * atr && c > vw) dir = 1;
                else if (-mom > st->param0 * atr && c < vw) dir = -1;
            }
            max_hold = 40;
            break;
        }
        case 8: { // smt_divergence (Cross-Asset Causal!)
            int lb = std::clamp(st->lookback, 5, 30);
            auto calc_z = [&](int which) -> double {
                double hh = -1e18, ll = 1e18, cc = 0.0;
                for (int i = 0; i < lb; ++i) {
                    const u11::RawBar& rb = (which == 0) ? view[-i] : ((which == 1) ? view.ref_nq_bar(-i) : ((which == 2) ? view.ref_es_bar(-i) : view.ref_ym_bar(-i)));
                    if (i == 0) cc = static_cast<double>(rb.c);
                    hh = std::max(hh, static_cast<double>(rb.h));
                    ll = std::min(ll, static_cast<double>(rb.l));
                }
                double rng = hh - ll;
                return (rng > 1e-6) ? ((cc - 0.5 * (hh + ll)) / rng) : 0.0;
            };
            double sz = calc_z(0);
            std::vector<double> pzs;
            if (view.has_ref_nq() && std::abs(st->inst.baseline_index - 4524.25) > 50.0) pzs.push_back(calc_z(1));
            if (view.has_ref_es() && std::abs(st->inst.baseline_index - 2100.0) > 50.0) pzs.push_back(calc_z(2));
            if (view.has_ref_ym() && std::abs(st->inst.baseline_index - 17777.0) > 50.0) pzs.push_back(calc_z(3));
            if (!pzs.empty()) {
                bool all_le0 = true, all_ge0 = true;
                for (double pz : pzs) { if (pz > 0.0) all_le0 = false; if (pz < 0.0) all_ge0 = false; }
                if (sz > st->param0 && all_le0) dir = -1;
                else if (sz < -st->param0 && all_ge0) dir = 1;
            }
            max_hold = 40;
            break;
        }
        case 9: { // turtle_soup (15m OR failure fade)
            if (view.or_formed(15)) {
                double orh = view.or_high(15), orl = view.or_low(15), mid = 0.5 * (orh + orl);
                double sw_up = h - orh, sw_dn = orl - l;
                if (sw_up >= st->param0 * atr && sw_up <= st->param1 * atr && c < orh && c > mid) {
                    dir = -1; sl_d = clamp_sl_pts(h - c + st->sl_base * atr, st->inst); tp_d = clamp_tp_pts(c - mid, st->inst);
                } else if (sw_dn >= st->param0 * atr && sw_dn <= st->param1 * atr && c > orl && c < mid) {
                    dir = 1; sl_d = clamp_sl_pts(c - l + st->sl_base * atr, st->inst); tp_d = clamp_tp_pts(mid - c, st->inst);
                }
            }
            break;
        }
        case 10: { // vsa_absorption
            if (rv > st->param0 && (h - l) < st->param1 * atr && st->hist.lows.count > 22) {
                double hh = -1e18, ll = 1e18;
                for (size_t i = 1; i <= 20; ++i) {
                    hh = std::max(hh, st->hist.highs.at(i));
                    ll = std::min(ll, st->hist.lows.at(i));
                }
                if (l <= ll && c > o) { dir = 1; sl_d = clamp_sl_pts(c - l + 0.15 * atr, st->inst); tp_d = clamp_tp_pts(1.5 * (h - l), st->inst); }
                else if (h >= hh && c < o) { dir = -1; sl_d = clamp_sl_pts(h - c + 0.15 * atr, st->inst); tp_d = clamp_tp_pts(1.5 * (h - l), st->inst); }
            }
            break;
        }
        case 11: { // vwap_multi_anchor
            double vw = view.session_vwap(0);
            if (st->hist.has_prior_day_vwap && st->hist.vwap_diffs.count >= 20) {
                double sd = std::max(0.25 * atr, st->hist.vwap_diffs.stdev());
                double z_rth = (c - vw) / sd;
                double z_pdc = (c - st->hist.prior_day_vwap) / sd;
                if (z_rth > st->param0 && z_pdc > st->param0 && st->hist.rsi7 < 70.0) dir = -1;
                else if (z_rth < -st->param0 && z_pdc < -st->param0 && st->hist.rsi7 > 30.0) dir = 1;
            }
            break;
        }
        case 12: { // bar_color_flip
            if (view.bar_index() >= 1) {
                bool prev_up = (view[-1].c > view[-1].o);
                bool cur_up  = (c > o);
                if (!prev_up && cur_up) dir = 1;
                else if (prev_up && !cur_up) dir = -1;
                sl_d = clamp_sl_pts(st->sl_base * ir, st->inst);
                tp_d = clamp_tp_pts(st->tp_base * ir, st->inst);
            }
            max_hold = 20;
            break;
        }
        case 13: { // bollinger_bands
            int n = std::clamp(st->lookback, 10, 40);
            double s = 0.0, s2 = 0.0;
            for (int i = 0; i < n; ++i) {
                double x = st->hist.closes.at(static_cast<size_t>(i));
                s += x; s2 += x * x;
            }
            double m = s / static_cast<double>(n);
            double sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(n) - m * m));
            if (c <= m - st->param0 * sd) dir = 1;
            else if (c >= m + st->param0 * sd) dir = -1;
            sl_d = clamp_sl_pts(st->sl_base * ir, st->inst);
            tp_d = clamp_tp_pts(st->tp_base * ir, st->inst);
            break;
        }
        case 14: { // ema_cross_scalp
            if (st->hist.closes.count >= 22) {
                double prev_e9 = st->hist.ema9 - (2.0 / 10.0) * (c - st->hist.closes.at(1));
                double prev_e21 = st->hist.ema21 - (2.0 / 22.0) * (c - st->hist.closes.at(1));
                if (prev_e9 <= prev_e21 && st->hist.ema9 > st->hist.ema21) dir = 1;
                else if (prev_e9 >= prev_e21 && st->hist.ema9 < st->hist.ema21) dir = -1;
                sl_d = clamp_sl_pts(st->sl_base * ir, st->inst);
                tp_d = clamp_tp_pts(st->tp_base * ir, st->inst);
            }
            max_hold = 30;
            break;
        }
        case 15: { // nitro_crt (strictly causal 15m HTF sweep & reclaim)
            if (st->hist.has_prev_m15 && st->hist.prev_m15_h > st->hist.prev_m15_l) {
                double ph = st->hist.prev_m15_h, pl = st->hist.prev_m15_l;
                if (l <= pl && c > pl) {
                    dir = 1; sl_d = clamp_sl_pts(c - pl + st->param0 * st->inst.tick_size, st->inst);
                    tp_d = clamp_tp_pts(st->tp_base * sl_d, st->inst);
                } else if (h >= ph && c < ph) {
                    dir = -1; sl_d = clamp_sl_pts(ph - c + st->param0 * st->inst.tick_size, st->inst);
                    tp_d = clamp_tp_pts(st->tp_base * sl_d, st->inst);
                }
            }
            break;
        }
        case 16: { // session_switch (London -> NY pullback market confirmation with 1.50/2.00 slippage)
            if (st->has_zone && em >= 510 && em < 600) {
                double limit = (st->zone_dir == 1) ? st->zone_hi : st->zone_lo;
                bool touched = (st->zone_dir == 1) ? (l <= limit) : (h >= limit);
                if (touched) {
                    double raw_sl = (st->zone_dir == 1) ? (c - st->zone_lo + atr) : (st->zone_hi - c + atr);
                    dir = st->zone_dir; ord_type = 0; trig_px = 0.0;
                    sl_d = clamp_sl_pts(raw_sl, st->inst);
                    tp_d = clamp_tp_pts(st->tp_base * sl_d, st->inst);
                    st->has_zone = false;
                }
            }
            max_hold = 90;
            break;
        }
        case 17: { // vwap_fade
            if (view.min_in_sess() >= 15) {
                double vw = view.session_vwap(0);
                double thr = st->param0 * ir;
                if (h >= vw + thr) dir = -1;
                else if (l <= vw - thr) dir = 1;
                sl_d = clamp_sl_pts(st->sl_base * ir, st->inst);
                tp_d = clamp_tp_pts(st->tp_base * ir, st->inst);
            }
            break;
        }
        case 18: { // reel_27_model_d_pdh_pdl_breakout_expansion
            if (st->hist.open5m_formed && view.has_prior_day() && em >= 575 && em < 660) {
                double c1 = static_cast<double>(view[-1].c);
                double c2 = static_cast<double>(view[-2].c);
                if (c1 > st->hist.open5m_h && c2 <= st->hist.open5m_h && c1 > view.pdh()) dir = 1;
                else if (c1 < st->hist.open5m_l && c2 >= st->hist.open5m_l && c1 < view.pdl()) dir = -1;
                sl_d = clamp_sl_pts(st->sl_base * ir, st->inst);
                tp_d = clamp_tp_pts(st->tp_base * ir, st->inst);
            }
            max_hold = 90;
            break;
        }
        case 19: { // qullamaggie_trading_system (trend + consolidation + rel_vol breakout)
            if (st->hist.closes.count >= 50 && rv >= st->param0) {
                double hh20 = -1e18, ll20 = 1e18;
                for (size_t i = 1; i <= 20; ++i) {
                    hh20 = std::max(hh20, st->hist.highs.at(i));
                    ll20 = std::min(ll20, st->hist.lows.at(i));
                }
                if (c > st->hist.ema50 && st->hist.ema9 > st->hist.ema20 && c > hh20) {
                    dir = 1; sl_d = clamp_sl_pts(std::min(st->sl_base * atr, c - l), st->inst);
                    tp_d = clamp_tp_pts(st->tp_base * sl_d, st->inst);
                } else if (c < st->hist.ema50 && st->hist.ema9 < st->hist.ema20 && c < ll20) {
                    dir = -1; sl_d = clamp_sl_pts(std::min(st->sl_base * atr, h - c), st->inst);
                    tp_d = clamp_tp_pts(st->tp_base * sl_d, st->inst);
                }
            }
            max_hold = 120;
            break;
        }
        case 20: { // fvg_execution_engine (3-bar FVG)
            if (view.bar_index() >= 3) {
                double min_gap = st->param0 * st->inst.tick_size;
                double bull_gap = static_cast<double>(view[-1].l) - static_cast<double>(view[-3].h);
                double bear_gap = static_cast<double>(view[-3].l) - static_cast<double>(view[-1].h);
                if (bull_gap >= min_gap) dir = 1;
                else if (bear_gap >= min_gap) dir = -1;
                sl_d = clamp_sl_pts(st->sl_base * ir, st->inst);
                tp_d = clamp_tp_pts(st->tp_base * ir, st->inst);
            }
            max_hold = 90;
            break;
        }
    }

    if (dir == 0) return sig;
    st->trades_today++;
    sig.direction = dir;
    sig.order_type = ord_type;
    sig.trigger_px = trig_px;
    sig.sl_pts = sl_d;
    sig.tp_pts = tp_d;
    if (st->engine_id == 19) {
        sig.ratchet_trig_pts = sl_d;
        sig.ratchet_trail_pts = std::max(st->inst.tick_size, 0.5 * sl_d);
    }
    sig.max_hold_bars = max_hold;
    sig.cooldown_bars = 1;
    return sig;
}

} // namespace batch1_detail

// ============================================================================
// PUBLIC BATCH 1 REGISTRATION (ALL 123 UIDs FROM batch1.json)
// ============================================================================
inline void register_batch1(std::vector<s546::RegisteredStrategy>& out) {
    using namespace batch1_detail;

    auto add_phys = [&](const char* uid, const char* name, const char* sf, const char* src,
                        const char* fid, int32_t mask, u11::ParamSchema sc,
                        void (*init_fn)(PhysicsFlagshipState&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, false, mask, std::move(sc),
            make_factory<PhysicsFlagshipState>(uid, physics_on_bar, init_fn)
        });
    };

    auto add_sniper = [&](const char* uid, const char* name, const char* sf, const char* src,
                          const char* fid, int32_t mask, u11::ParamSchema sc,
                          void (*init_fn)(SniperVaultState&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, false, mask, std::move(sc),
            make_factory<SniperVaultState>(uid, sniper_on_bar, init_fn)
        });
    };

    auto add_orb = [&](const char* uid, const char* name, const char* sf, const char* src,
                       const char* fid, int32_t mask, u11::ParamSchema sc,
                       void (*init_fn)(OrbGenState&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, false, mask, std::move(sc),
            make_factory<OrbGenState>(uid, orb_on_bar, init_fn)
        });
    };

    auto add_t1 = [&](const char* uid, const char* name, const char* sf, const char* src,
                      const char* fid, int32_t mask, u11::ParamSchema sc,
                      void (*init_fn)(Tier1State&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, false, mask, std::move(sc),
            make_factory<Tier1State>(uid, tier1_on_bar, init_fn)
        });
    };

    auto add_asian = [&](const char* uid, const char* name, const char* sf, const char* src,
                         const char* fid, int32_t mask, u11::ParamSchema sc,
                         void (*init_fn)(AsianSuiteState&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, false, mask, std::move(sc),
            make_factory<AsianSuiteState>(uid, asian_on_bar, init_fn)
        });
    };

    auto add_paper = [&](const char* uid, const char* name, const char* sf, const char* src,
                         const char* fid, int32_t mask, u11::ParamSchema sc,
                         void (*init_fn)(PaperSuiteState&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, false, mask, std::move(sc),
            make_factory<PaperSuiteState>(uid, paper_on_bar, init_fn)
        });
    };

    auto add_quant = [&](const char* uid, const char* name, const char* sf, const char* src,
                         const char* fid, bool cross, int32_t mask, u11::ParamSchema sc,
                         void (*init_fn)(QuantStandaloneState&, const std::vector<double>&, const u11::InstrumentSpec*)) {
        out.push_back(RegisteredStrategy{
            uid, name, sf, src, fid, cross, mask, std::move(sc),
            make_factory<QuantStandaloneState>(uid, quant_on_bar, init_fn)
        });
    };

    auto phys_schema = [](double sl, double tp, double rt, double rd, double buf,
                          double t159, double t151, double t157, double ext, int mx) {
        return u11::ParamSchema{{
            p_lin("sl_pts", sl, std::min(sl, std::max(2.0, 0.5 * sl)), std::max(12.0, 2.0 * sl), 5),
            p_lin("tp_pts", tp, std::min(tp, 12.0), std::max(100.0, 1.5 * tp), 5),
            p_lin("ratchet_trig_pts", rt, std::min(rt, 0.25), std::max(5.0, 2.5 * rt), 5),
            p_lin("ratchet_trail_pts", rd, std::min(rd, 0.25), std::max(3.0, 2.5 * rd), 5),
            p_lin("or_buf_pts", buf, std::min(buf, 0.0), std::max(5.0, 2.0 * buf), 4),
            p_lin("th_159", t159, std::min(t159, 0.8), std::max(3.0, 1.5 * t159), 4),
            p_lin("th_151", t151, std::min(t151, 0.4), std::max(1.0, 1.5 * t151), 4),
            p_lin("th_157", t157, std::min(t157, 1.5), std::max(5.0, 1.5 * t157), 4),
            p_lin("extra_param", ext, std::min(ext, 0.0), std::max(15.0, 1.5 * ext), 4),
            p_int("max_entries", mx, std::min(mx, 2), std::max(20, mx), 4)
        }};
    };

    // ------------------------------------------------------------------------
    // 1. Flagship Multi-Leg & Vault Snipers (13 UIDs)
    // ------------------------------------------------------------------------
    add_phys("000_grand_champion_trio", "Grand Champion Trio Netting Execution Engine",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "grand_champion_trio/engine.cpp", "EXACT_SOURCE",
             0x7, phys_schema(36.0, 60.0, 2.50, 1.25, 1.0, 1.50, 0.80, 3.00, 0.0, 12), init_physics_v<0>);
    add_phys("001_champion_quad_execution", "Champion-Quad Execution Strategy",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "champion_quad_engine.cpp", "EXACT_SOURCE",
             0x7, phys_schema(36.0, 60.0, 1.50, 0.75, 1.0, 1.50, 0.80, 3.00, 0.0, 12), init_physics_v<1>);
    add_phys("002_matrix_12leg_decoupled_execution", "Matrix-12Leg Decoupled Microstructure Engine",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "matrix_12leg_decoupled_engine.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 0.0, 12), init_physics_v<2>);
    add_phys("002_matrix_12leg_interbar_decoupled_execution", "002 Matrix-12Leg Interbar Decoupled Execution (#31412)",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "Matrix_12Leg_Interbar_Decoupled_Execution_core.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 0.50, 12), init_physics_v<3>);
    add_phys("002_matrix_12leg_intrabar_decoupled_execution", "002 Matrix-12Leg Intrabar Decoupled Execution (#31395)",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "Matrix_12Leg_Intrabar_Decoupled_Execution_core.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 0.50, 12), init_physics_v<4>);
    add_phys("002_matrix_12leg_decoupled_safety_king", "002 Matrix-12Leg Decoupled Safety King (#389329964)",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "matrix_12leg_decoupled_safety_king_engine.cpp", "EXACT_SOURCE",
             0x7, phys_schema(6.5, 60.0, 2.00, 0.50, 1.0, 1.50, 0.80, 3.00, 0.0, 12), init_physics_v<5>);
    add_phys("002_matrix_12leg_decoupled_velocity_demon", "002 Matrix-12Leg Decoupled Velocity Demon (#1551249409)",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "Matrix_12Leg_Decoupled_Velocity_Demon_core.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 0.0, 12), init_physics_v<6>);
    add_phys("003_velocity_demon_dalton_ib_champion_unified", "Velocity Demon + Dalton IB Midpoint Fade Unified Champion",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "Unified_Champion_Execution_core.cpp", "EXACT_SOURCE",
             0xF, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 2.0, 12), init_physics_v<7>);

    add_sniper("004_dalton_ib_midpoint_fade_execution", "Dalton Initial Balance (IB) Midpoint Fade",
               "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "Dalton_IB_Midpoint_Fade_core.cpp", "EXACT_SOURCE",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5),
                   p_lin("rr_mult", 1.667, 1.0, 3.0, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4),
                   p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 60, 60, 60, 2),
                   p_int("max_per_day", 2, 1, 4, 4)
               }}, init_sniper_v<0>);
    add_sniper("005_reel_46_trend_continuation_sniper", "Reel 46 Trend Continuation Sniper (Cheatcodes 4H Fractal)",
               "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "cheatcodes_4h_fractal.py", "EXACT_SOURCE",
               0x4, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 3.0, 1.5, 4.0, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 240, 120, 240, 3), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<1>);
    add_sniper("006_reel_29_orb_momentum_sniper", "Reel 29 ORB Momentum Sniper (Dodgy Resting Orders Sweep)",
               "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "dodgy_resting_sweep.py", "EXACT_SOURCE",
               0x4, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.0, 1.0, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 60, 30, 120, 4), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<2>);
    add_sniper("007_reel_39_asian_midpoint_sniper", "Reel 39 Asian Midpoint Sniper (Creative CISD Confirmation)",
               "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "creative_confirmation.py", "EXACT_SOURCE",
               0x1, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.0, 1.0, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 15, 10, 30, 4), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<3>);

    add_phys("008_booster_champion_breakout_execution", "Booster Champion Breakout Execution Strategy",
             "01_Flagship_MultiLeg_and_Vault_Snipers_000_008", "Booster_Champion_Breakout_core.cpp", "EXACT_SOURCE",
             0xF, phys_schema(8.0, 60.0, 1.00, 0.375, 1.0, 1.50, 0.80, 3.00, 8.0, 12), init_physics_v<8>);

    // ------------------------------------------------------------------------
    // 2. ORB & Pullback Generations (19 UIDs)
    // ------------------------------------------------------------------------
    auto orb_schema = [](int or_m, double buf, double sl, double rr, double rtr, double rtd, int mx, int skip, double sm) {
        return u11::ParamSchema{{
            p_disc("or_mins", static_cast<double>(or_m), {15.0, 30.0, 45.0, 60.0}),
            p_lin("buf_base", buf, std::min(buf, 5.0), std::max(60.0, 1.5 * buf), 5),
            p_lin("base_sl_pts", sl, std::min(sl, 12.0), std::max(120.0, 1.5 * sl), 5),
            p_lin("rr_tp", rr, 0.0, std::max(3.0, 1.5 * rr), 4),
            p_lin("ratchet_trig_r", rtr, std::min(rtr, 0.1), std::max(1.5, 2.0 * rtr), 4),
            p_lin("ratchet_trail_r", rtd, std::min(rtd, 0.1), std::max(1.0, 2.0 * rtd), 4),
            p_int("max_entries", mx, 1, std::max(15, mx), 4),
            p_int("min_entry_idx", skip, 0, std::max(3, skip), 4),
            p_lin("stretch_mult", sm, 0.5, 2.0, 4)
        }};
    };

    add_orb("cimi_orb", "Andrea Cimi Quantitative ORB Suite", "03_ORB_and_Pullback_Generations", "cimi_orb.py", "EXACT_SOURCE",
            0xC, orb_schema(30, 0.0, 20.0, 1.0, 0.0, 0.0, 1, 0, 1.0), init_orb_v<0>);
    add_orb("crabel_orb_stretch", "Toby Crabel ORB + Stretch (ID / NR4 / NR7 / IDNR4)", "03_ORB_and_Pullback_Generations", "crabel_orb.py", "EXACT_SOURCE",
            0x4, orb_schema(15, 0.0, 20.0, 0.0, 0.0, 0.0, 1, 0, 1.0), init_orb_v<1>);
    add_orb("flexing_joe_orb", "Flexing Joe ORB + Counter Strategy", "03_ORB_and_Pullback_Generations", "flexing_joe_orb.py", "EXACT_SOURCE",
            0x4, orb_schema(30, 0.0, 25.0, 2.0, 0.0, 0.0, 2, 0, 1.0), init_orb_v<2>);
    add_orb("kasen_midpoint_retest", "Kasen Midpoint-Retest ORB Strategy", "03_ORB_and_Pullback_Generations", "kasen_midpoint_retest.py", "EXACT_SOURCE",
            0xC, orb_schema(15, 10.0, 15.0, 0.0, 0.50, 0.25, 12, 0, 1.0), init_orb_v<3>);
    add_orb("orb_cooptimized_champions", "ORB Co-Optimized Champions Suite", "03_ORB_and_Pullback_Generations", "orb_v6_5.py", "EXACT_FORMULA",
            0xF, orb_schema(15, 30.0, 50.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<5>);
    add_orb("orb_live_runner_v1_original", "ORB Live Runner v1 Original Bot", "03_ORB_and_Pullback_Generations", "orb_v5.py", "EXACT_FORMULA",
            0xC, orb_schema(15, 20.0, 25.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<4>);
    add_orb("orb_pullback_cooptimized_champions", "ORB Pullback Co-Optimized Champions", "03_ORB_and_Pullback_Generations", "orb_pullback_v2_5.py", "EXACT_FORMULA",
            0xC, orb_schema(15, 20.0, 36.0, 0.0, 0.50, 0.25, 12, 0, 1.0), init_orb_v<8>);
    add_orb("orb_pullback_scalper", "ORB Pullback Scalper (v1 Baseline)", "03_ORB_and_Pullback_Generations", "orb_pullback_scalper.py", "EXACT_SOURCE",
            0xC, orb_schema(15, 20.0, 15.0, 0.0, 0.50, 0.25, 12, 0, 1.0), init_orb_v<8>);
    add_orb("orb_pullback_v2_5", "ORB Pullback v2.5 (100-Domain Kinematic Elastic Stop)", "03_ORB_and_Pullback_Generations", "orb_pullback_v2_5.py", "EXACT_SOURCE",
            0xC, orb_schema(15, 20.0, 36.0, 0.0, 0.50, 0.25, 12, 0, 1.0), init_orb_v<8>);
    add_orb("orb_v5_5", "ORB v5.5 (20*S Buffer + 100 Scientific Stop Domains)", "03_ORB_and_Pullback_Generations", "orb_v5_5.py", "EXACT_SOURCE",
            0xF, orb_schema(15, 20.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<5>);
    add_orb("orb_v6_5", "ORB v6.5 (Pre-Market ATR Buffer + Orthogonal Theorems)", "03_ORB_and_Pullback_Generations", "orb_v6_5.py", "EXACT_SOURCE",
            0xF, orb_schema(15, 40.0, 50.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<7>);
    add_orb("original_orb", "Original ORB (Legacy Baseline)", "03_ORB_and_Pullback_Generations", "original_orb.py", "EXACT_SOURCE",
            0xC, orb_schema(15, 20.0, 20.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<4>);
    add_orb("trade_slices_combinatorial_suite", "Trade Slices Combinatorial Execution Gate (Trades 2-3 / 4-8)", "03_ORB_and_Pullback_Generations", "orb_v6.py", "EXACT_FORMULA",
            0xF, orb_schema(15, 25.0, 36.0, 0.0, 0.50, 0.25, 8, 2, 1.0), init_orb_v<7>);
    add_orb("gdrive_eval_speed_champions_mc_suite", "Topstep Eval Speed Champions Suite", "03_ORB_and_Pullback_Generations", "orb_v6_5.py", "EXACT_FORMULA",
            0xF, orb_schema(15, 35.0, 36.0, 0.0, 0.50, 0.25, 3, 1, 1.0), init_orb_v<7>);
    add_orb("kernel_reel_7", "Usman Ashraf 15m Classic ORB (Reel 7)", "03_ORB_and_Pullback_Generations", "usman_ashraf_orb.py", "EXACT_SOURCE",
            0x4, orb_schema(15, 0.0, 30.0, 2.0, 0.0, 0.0, 1, 0, 1.0), init_orb_v<9>);
    add_orb("orb_pullback_v2", "ORB Pullback v2 (Dynamic Agile OR-Span Buffer)", "03_ORB_and_Pullback_Generations", "orb_pullback_v2.py", "EXACT_SOURCE",
            0xC, orb_schema(15, 20.0, 15.0, 0.0, 0.50, 0.25, 12, 0, 1.0), init_orb_v<8>);
    add_orb("orb_v6", "ORB v6 (1H Pre-Market ATR Dynamic Scaling)", "03_ORB_and_Pullback_Generations", "orb_v6.py", "EXACT_SOURCE",
            0xF, orb_schema(15, 25.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<6>);
    add_orb("gdrive_phase2_funded_payout_champions_suite", "Phase-2 Funded Account Payout Champions Matrix", "03_ORB_and_Pullback_Generations", "orb_v6_5.py", "EXACT_FORMULA",
            0xF, orb_schema(15, 40.0, 50.0, 0.0, 0.50, 0.25, 3, 1, 1.0), init_orb_v<7>);
    add_orb("orb_v5", "ORB v5 Multi-Timeframe Strategy", "03_ORB_and_Pullback_Generations", "orb_v5.py", "EXACT_SOURCE",
            0xF, orb_schema(15, 36.0, 36.0, 0.0, 0.50, 0.25, 12, 1, 1.0), init_orb_v<4>);

    // ------------------------------------------------------------------------
    // 3. Tier-1 Multiverse Suites (23 UIDs)
    // ------------------------------------------------------------------------
    auto t1_schema = [](double buf_m, double sl, double tp_m, int lb, int max_tr, double er_th = 0.60) {
        return u11::ParamSchema{{
            p_lin("buf_m", buf_m, std::min(buf_m, 0.05), std::max(0.75, 1.5 * buf_m), 5),
            p_lin("sl_base_pts", sl, std::min(sl, 20.0), std::max(140.0, 1.2 * sl), 5),
            p_lin("tp_m", tp_m, std::min(tp_m, 1.0), std::max(3.5, 1.5 * tp_m), 5),
            p_int("param_lb", lb, std::min(lb, 15), std::max(60, lb), 4),
            p_int("max_trades", max_tr, 1, std::max(3, max_tr), 3),
            p_lin("er_thresh", er_th, std::min(er_th, 0.30), std::max(0.80, er_th), 4)
        }};
    };

    add_t1("tier1_dalton_ib", "Dalton Initial Balance (IB) Fade — Tier 1 Suite", "04_Tier1_Multiverse_Suites",
           "tier1_ground_truth_engine.py", "EXACT_SOURCE", 0xF, t1_schema(0.35, 100.0, 1.5, 60, 2), init_tier1_v<0, 0xF, 0>);
    add_t1("tier1_ker_adaptive", "KER Adaptive Switcher (Kaufman Efficiency Ratio)", "04_Tier1_Multiverse_Suites",
           "tier1_ground_truth_engine.py", "EXACT_SOURCE", 0xC, t1_schema(0.20, 25.0, 2.0, 15, 2, 0.60), init_tier1_v<2, 0xC, 1>);
    add_t1("tier1_lead_lag_mom", "Lead-Lag Momentum Pullback (Option C Dual-Exit)", "04_Tier1_Multiverse_Suites",
           "tier1_ground_truth_engine.py", "EXACT_SOURCE", 0xF, t1_schema(0.10, 60.0, 1.5, 30, 2), init_tier1_v<1, 0xF, 0>);
    add_t1("tier1_london_reset", "Tier-1 London / Prior Session Reset", "04_Tier1_Multiverse_Suites",
           "tier1_multiverse_master_sweep_v2.cpp", "EXACT_FORMULA", 0xE, t1_schema(0.25, 40.0, 1.5, 30, 2), init_tier1_v<3, 0xE, 2>);

    // Tier-1 19 Champions (exact parameters from tier1_19_champions_manifest.json)
    add_t1("tier1_champ_01_dalton_ib_nq_ny", "Tier-1 Champ #01: Dalton_IB (NQ - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.35, 100.0, 1.5, 60, 2), init_tier1_v<0, 0xC, 0>);
    add_t1("tier1_champ_02_lead_lag_mom_nq_ny", "Tier-1 Champ #02: Lead_Lag_Mom (NQ - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.10, 60.0, 1.5, 30, 2), init_tier1_v<1, 0xC, 0>);
    add_t1("tier1_champ_03_ker_adaptive_nq_ny", "Tier-1 Champ #03: KER_Adaptive (NQ - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.20, 25.0, 2.0, 15, 2, 0.60), init_tier1_v<2, 0xC, 1>);
    add_t1("tier1_champ_04_dalton_ib_nq_london", "Tier-1 Champ #04: Dalton_IB (NQ - London)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x2, t1_schema(0.50, 75.0, 1.5, 60, 3), init_tier1_v<0, 0x2, 0>);
    add_t1("tier1_champ_05_lead_lag_mom_nq_london", "Tier-1 Champ #05: Lead_Lag_Mom (NQ - London)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x2, t1_schema(0.50, 25.0, 2.0, 45, 2), init_tier1_v<1, 0x2, 0>);
    add_t1("tier1_champ_06_dalton_ib_nq_asian", "Tier-1 Champ #06: Dalton_IB (NQ - Asian)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x1, t1_schema(0.25, 50.0, 1.5, 60, 3), init_tier1_v<0, 0x1, 0>);
    add_t1("tier1_champ_07_lead_lag_mom_nq_asian", "Tier-1 Champ #07: Lead_Lag_Mom (NQ - Asian)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x1, t1_schema(0.50, 60.0, 2.5, 20, 2), init_tier1_v<1, 0x1, 0>);
    add_t1("tier1_champ_08_dalton_ib_es_ny", "Tier-1 Champ #08: Dalton_IB (ES - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.05, 75.0, 3.0, 60, 1), init_tier1_v<0, 0xC, 0>);
    add_t1("tier1_champ_09_lead_lag_mom_es_ny", "Tier-1 Champ #09: Lead_Lag_Mom (ES - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.35, 25.0, 3.0, 30, 2), init_tier1_v<1, 0xC, 0>);
    add_t1("tier1_champ_10_dalton_ib_es_london", "Tier-1 Champ #10: Dalton_IB (ES - London)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x2, t1_schema(0.25, 25.0, 3.0, 60, 3), init_tier1_v<0, 0x2, 0>);
    add_t1("tier1_champ_11_lead_lag_mom_es_london", "Tier-1 Champ #11: Lead_Lag_Mom (ES - London)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x2, t1_schema(0.10, 100.0, 2.0, 60, 1), init_tier1_v<1, 0x2, 0>);
    add_t1("tier1_champ_12_dalton_ib_es_asian", "Tier-1 Champ #12: Dalton_IB (ES - Asian)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x1, t1_schema(0.15, 120.0, 1.5, 60, 2), init_tier1_v<0, 0x1, 0>);
    add_t1("tier1_champ_13_lead_lag_mom_es_asian", "Tier-1 Champ #13: Lead_Lag_Mom (ES - Asian)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x1, t1_schema(0.50, 40.0, 3.0, 20, 2), init_tier1_v<1, 0x1, 0>);
    add_t1("tier1_champ_14_dalton_ib_ym_ny", "Tier-1 Champ #14: Dalton_IB (YM - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.50, 120.0, 2.5, 60, 3), init_tier1_v<0, 0xC, 0>);
    add_t1("tier1_champ_15_lead_lag_mom_ym_ny", "Tier-1 Champ #15: Lead_Lag_Mom (YM - NY)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0xC, t1_schema(0.50, 100.0, 1.5, 20, 2), init_tier1_v<1, 0xC, 0>);
    add_t1("tier1_champ_16_dalton_ib_ym_london", "Tier-1 Champ #16: Dalton_IB (YM - London)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x2, t1_schema(0.05, 50.0, 3.0, 60, 3), init_tier1_v<0, 0x2, 0>);
    add_t1("tier1_champ_17_lead_lag_mom_ym_london", "Tier-1 Champ #17: Lead_Lag_Mom (YM - London)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x2, t1_schema(0.10, 40.0, 2.0, 60, 2), init_tier1_v<1, 0x2, 0>);
    add_t1("tier1_champ_18_dalton_ib_ym_asian", "Tier-1 Champ #18: Dalton_IB (YM - Asian)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x1, t1_schema(0.05, 100.0, 3.0, 60, 3), init_tier1_v<0, 0x1, 0>);
    add_t1("tier1_champ_19_lead_lag_mom_ym_asian", "Tier-1 Champ #19: Lead_Lag_Mom (YM - Asian)", "04_Tier1_Multiverse_Suites",
           "tier1_19_champions_manifest.json", "EXACT_SOURCE", 0x1, t1_schema(0.50, 80.0, 3.0, 20, 2), init_tier1_v<1, 0x1, 0>);

    // ------------------------------------------------------------------------
    // 4. Social Alpha Mean Reversion Suite (6 UIDs)
    // ------------------------------------------------------------------------
    add_sniper("social_alpha_cheatcodes_4h_fractal", "Social Alpha: Cheatcodes 4H Fractal Acceptance",
               "07_Social_Alpha_Mean_Reversion_Suite", "cheatcodes_4h_fractal.py", "EXACT_SOURCE",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 3.0, 1.5, 4.0, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 240, 120, 240, 3), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<4>);
    add_sniper("social_alpha_creative_confirmation", "Social Alpha: Creative CISD Confirmation Model",
               "07_Social_Alpha_Mean_Reversion_Suite", "creative_confirmation.py", "EXACT_SOURCE",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.0, 1.0, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 15, 10, 30, 4), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<5>);
    add_sniper("social_alpha_dodgy_resting_sweep", "Social Alpha: Dodgy Resting Orders Sweep",
               "07_Social_Alpha_Mean_Reversion_Suite", "dodgy_resting_sweep.py", "EXACT_SOURCE",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.0, 1.0, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 60, 30, 120, 4), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<6>);
    add_sniper("social_alpha_portfolio_apex_1m_trio", "Social Alpha Portfolio: apex_1m_trio",
               "07_Social_Alpha_Mean_Reversion_Suite", "MULTI_REEL_PORTFOLIO_AND_CORRELATION.json", "EXACT_FORMULA",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.5, 1.5, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 60, 15, 240, 4), p_int("max_per_day", 5, 2, 8, 4)
               }}, init_sniper_v<7>);
    add_sniper("social_alpha_portfolio_best_tf_champions", "Social Alpha Portfolio: best_tf_champions",
               "07_Social_Alpha_Mean_Reversion_Suite", "MULTI_REEL_PORTFOLIO_AND_CORRELATION.json", "EXACT_FORMULA",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.5, 1.5, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 60, 15, 240, 4), p_int("max_per_day", 5, 2, 8, 4)
               }}, init_sniper_v<8>);
    add_sniper("social_alpha_portfolio_mega_stack_1m_3m_5m", "Social Alpha Portfolio: mega_stack_1m_3m_5m",
               "07_Social_Alpha_Mean_Reversion_Suite", "MULTI_REEL_PORTFOLIO_AND_CORRELATION.json", "EXACT_FORMULA",
               0xC, u11::ParamSchema{{
                   p_lin("sl_pts", 12.0, 6.0, 24.0, 5), p_lin("rr_mult", 2.5, 1.5, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 60, 15, 240, 4), p_int("max_per_day", 6, 2, 10, 4)
               }}, init_sniper_v<9>);

    // ------------------------------------------------------------------------
    // 5. Topstep Paper1, Paper2 & Asian Matrices (31 UIDs)
    // ------------------------------------------------------------------------
    auto asian_schema = [](double sl, double tp, int lb, double pa, double pb, int mx) {
        return u11::ParamSchema{{
            p_lin("sl_atr", sl, 0.5, 5.0, 5),
            p_lin("tp_atr", tp, 0.25, 3.0, 5),
            p_int("lookback", lb, std::min(10, lb), std::max(55, lb + 10), 4),
            p_lin("param_a", pa, std::max(0.2, 0.5 * pa), std::max(3.0, 1.5 * pa), 4),
            p_lin("param_b", pb, std::max(0.2, 0.5 * pb), std::max(3.0, 1.2 * pb), 4),
            p_int("max_per_sess", mx, 1, std::max(3, mx), 3)
        }};
    };

    add_asian("topstep_asian_session_atr_compression_failed_breakout", "Asian Session MR: atr_compression_failed_breakout",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_session.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 10, 0.6, 1.0, 1), init_asian_v<0, 0x1>);
    add_asian("topstep_asian_session_bollinger_mean_reversion", "Asian Session MR: bollinger_mean_reversion",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_session.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 20, 2.0, 1.0, 1), init_asian_v<1, 0x1>);
    add_asian("topstep_asian_session_opening_range_fade", "Asian Session MR: opening_range_fade",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_session.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 30, 1.0, 1.0, 1), init_asian_v<2, 0x1>);
    add_asian("topstep_asian_session_pivot_range_scalp", "Asian Session MR: pivot_range_scalp",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_session.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 90, 1.0, 1.0, 1), init_asian_v<3, 0x1>);
    add_asian("topstep_asian_session_rsi_extreme", "Asian Session MR: rsi_extreme",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_session.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 14, 20.0, 80.0, 1), init_asian_v<4, 0x1>);
    add_asian("topstep_asian_session_vwap_deviation", "Asian Session MR: vwap_deviation",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_session.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 20, 1.5, 1.0, 1), init_asian_v<5, 0x1>);

    add_asian("topstep_asian_paper_kathy_lien_dbb", "Asian Paper Engine 1: kathy_lien_dbb",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 20, 2.0, 1.0, 1), init_asian_v<6, 0x1>);
    add_asian("topstep_asian_paper_rayner_teo_false_breakout", "Asian Paper Engine 2: rayner_teo_false_breakout",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 20, 1.5, 1.0, 1), init_asian_v<7, 0x1>);
    add_asian("topstep_asian_paper_tradeciety_bollinger_rsi", "Asian Paper Engine 3: tradeciety_bollinger_rsi",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 20, 2.5, 1.0, 1), init_asian_v<8, 0x1>);
    add_asian("topstep_asian_paper_stacey_burke_3push", "Asian Paper Engine 4: stacey_burke_3push",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 20, 1.0, 1.0, 1), init_asian_v<9, 0x1>);
    add_asian("topstep_asian_paper_trading_rush_hma_macd", "Asian Paper Engine 5: trading_rush_hma_macd",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.5, 55, 1.0, 1.0, 1), init_asian_v<10, 0x1>);
    add_asian("topstep_asian_paper_larry_williams_pr", "Asian Paper Engine 6: larry_williams_pr",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 14, 80.0, 75.0, 1), init_asian_v<11, 0x1>);
    add_asian("topstep_asian_paper_ukspreadbetting_camarilla", "Asian Paper Engine 7: ukspreadbetting_camarilla",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 15, 1.0, 1.0, 1), init_asian_v<12, 0x1>);
    add_asian("topstep_asian_paper_ezekiel_chew_pin_bar", "Asian Paper Engine 8: ezekiel_chew_pin_bar",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 60, 2.0, 1.0, 1), init_asian_v<13, 0x1>);
    add_asian("topstep_asian_paper_trade_prime_kdj", "Asian Paper Engine 9: trade_prime_kdj",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "asian_paper_matrix.py", "EXACT_SOURCE", 0x1, asian_schema(2.0, 1.0, 30, 12.0, 1.0, 1), init_asian_v<14, 0x1>);

    auto paper_schema = [](double sl, double tp, int lb, double pa, double pb) {
        return u11::ParamSchema{{
            p_lin("sl_atr", sl, std::min(sl, 1.0), std::max(6.0, 1.5 * sl), 5),
            p_lin("tp_atr", tp, std::min(tp, 0.5), std::max(4.0, 1.5 * tp), 5),
            p_int("lookback", lb, std::min(lb, 10), std::max(40, lb), 4),
            p_lin("param_a", pa, std::min(pa, 0.3), std::max(3.0, 1.5 * pa), 4),
            p_lin("param_b", pb, std::min(pb, 15.0), std::max(40.0, 1.2 * pb), 4),
            p_int("max_per_day", 1, 1, 3, 3)
        }};
    };

    add_paper("topstep_paper1_ict_silver_bullet", "Paper 1 Blueprint 1: ict_silver_bullet",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 15, 0.5, 20.0), init_paper_v<0>);
    add_paper("topstep_paper1_casper_inverted_fvg", "Paper 1 Blueprint 2: casper_inverted_fvg",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 15, 1.0, 20.0), init_paper_v<1>);
    add_paper("topstep_paper1_velez_20_200_elephant_bar", "Paper 1 Blueprint 3: velez_20_200_elephant_bar",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 20, 1.0, 20.0), init_paper_v<2>);
    add_paper("topstep_paper1_rosato_sd_absorption", "Paper 1 Blueprint 4: rosato_sd_absorption",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 15, 1.0, 20.0), init_paper_v<3>);
    add_paper("topstep_paper1_carter_ttm_squeeze", "Paper 1 Blueprint 5: carter_ttm_squeeze",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 20, 1.5, 20.0), init_paper_v<4>);
    add_paper("topstep_paper1_raschke_holy_grail", "Paper 1 Blueprint 6: raschke_holy_grail",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 20, 0.5, 25.0), init_paper_v<5>);
    add_paper("topstep_paper1_wade_pats_second_entry", "Paper 1 Blueprint 7: wade_pats_second_entry",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper1_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(5.0, 2.0, 10, 1.0, 20.0), init_paper_v<6>);

    add_paper("topstep_paper2_liquidity_purge_2022", "Paper 2 Engine 1: liquidity_purge_2022",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 10, 0.5, 20.0), init_paper_v<7>);
    add_paper("topstep_paper2_structural_confluence_unicorn", "Paper 2 Engine 2: structural_confluence_unicorn",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 21, 1.5, 30.0), init_paper_v<8>);
    add_paper("topstep_paper2_smt_divergence", "Paper 2 Engine 3: smt_divergence",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 20, 1.0, 20.0), init_paper_v<9>);
    add_paper("topstep_paper2_tick_price_action_2000", "Paper 2 Engine 4: tick_price_action_2000",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 5, 0.5, 20.0), init_paper_v<10>);
    add_paper("topstep_paper2_vpoc_reversion", "Paper 2 Engine 5: vpoc_reversion",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 30, 1.5, 20.0), init_paper_v<11>);
    add_paper("topstep_paper2_level3_footprint_absorption", "Paper 2 Engine 6: level3_footprint_absorption",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 20, 0.3, 20.0), init_paper_v<12>);
    add_paper("topstep_paper2_atr_fair_value_scalp", "Paper 2 Engine 7: atr_fair_value_scalp",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 20, 1.5, 20.0), init_paper_v<13>);
    add_paper("topstep_paper2_vwap_zscore_orb", "Paper 2 Engine 8: vwap_zscore_orb",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 15, 1.0, 20.0), init_paper_v<14>);
    add_paper("topstep_paper2_algorithmic_indicator_convergence", "Paper 2 Engine 9: algorithmic_indicator_convergence",
              "09_Topstep_Paper1_Paper2_Asian_Matrices", "paper2_matrix.py", "EXACT_SOURCE", 0xF, paper_schema(3.0, 2.0, 20, 1.2, 25.0), init_paper_v<15>);

    // ------------------------------------------------------------------------
    // 6. Topstep Quant Suite & Standalones (23 UIDs)
    // ------------------------------------------------------------------------
    add_asian("asian_paper_grid_suite", "Asian Paper 9-Engine Multi-Session ATR Grid Suite",
              "10_Topstep_Quant_Suite_and_Standalones", "asian_paper_matrix.py", "EXACT_FORMULA", 0xF, asian_schema(2.0, 0.75, 20, 2.0, 1.0, 2), init_asian_v<15, 0xF>);

    auto q_schema = [](double p0, double p1, int lb, double sl, double tp, int mx) {
        return u11::ParamSchema{{
            p_lin("param0", p0, std::min(p0, std::max(0.05, 0.4 * p0)), std::max(1.5, 2.5 * p0), 4),
            p_lin("param1", p1, std::min(p1, std::max(0.0, 0.4 * p1)), std::max(1.5, 2.5 * p1), 4),
            p_int("lookback", lb, std::min(5, lb), std::max(50, lb), 4),
            p_lin("sl_param", sl, std::min(sl, std::max(0.15, 0.5 * sl)), std::max(2.0, 2.0 * sl), 5),
            p_lin("tp_param", tp, std::min(tp, std::max(0.5, 0.5 * tp)), std::max(3.0, 2.0 * tp), 5),
            p_int("max_per_day", mx, 1, std::max(10, mx), 4)
        }};
    };

    add_quant("bar_color_flip", "Bar Color Flip Scalp Adapter", "10_Topstep_Quant_Suite_and_Standalones",
              "test_scalp_strategies.py", "EXACT_SOURCE", false, 0xF, q_schema(1.0, 1.0, 5, 10.0, 10.0, 8), init_quant_v<12, 0xF>);
    add_quant("bollinger_bands", "Bollinger Bands Mean Reversion", "10_Topstep_Quant_Suite_and_Standalones",
              "bollinger_fade.py", "EXACT_SOURCE", false, 0xF, q_schema(2.0, 1.0, 20, 15.0, 15.0, 6), init_quant_v<13, 0xF>);
    add_quant("ema_cross_scalp", "EMA Cross Scalp Adapter", "10_Topstep_Quant_Suite_and_Standalones",
              "test_scalp_strategies.py", "EXACT_SOURCE", false, 0xF, q_schema(9.0, 21.0, 21, 12.0, 18.0, 6), init_quant_v<14, 0xF>);
    add_quant("nitro_crt", "Nitro CRT Strategy (Causal Clean-Room)", "10_Topstep_Quant_Suite_and_Standalones",
              "nitro_crt.py", "EXACT_FORMULA", false, 0xC, q_schema(2.0, 1.0, 15, 1.0, 2.0, 1), init_quant_v<15, 0xC>);

    add_quant("topstep_quant_suite12_amt_value", "Topstep Quant Suite 12: amt_value", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(1.3, 1.0, 20, 0.5, 2.0, 3), init_quant_v<0, 0xF>);
    add_quant("topstep_quant_suite12_bb_kc_squeeze", "Topstep Quant Suite 12: bb_kc_squeeze", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(2.0, 1.5, 20, 1.5, 4.5, 3), init_quant_v<1, 0xF>);
    add_quant("topstep_quant_suite12_beta_leadlag", "Topstep Quant Suite 12: beta_leadlag", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "CROSS_ASSET_CAUSAL", true, 0xF, q_schema(0.20, 0.0, 15, 1.0, 1.0, 4), init_quant_v<2, 0xF>);
    add_quant("topstep_quant_suite12_climax_snapback", "Topstep Quant Suite 12: climax_snapback", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(3.5, 1.0, 14, 1.0, 2.0, 3), init_quant_v<3, 0xF>);
    add_quant("topstep_quant_suite12_ker_regime", "Topstep Quant Suite 12: ker_regime", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(0.60, 0.25, 20, 1.0, 2.0, 4), init_quant_v<4, 0xF>);
    add_quant("topstep_quant_suite12_kinetic_velocity", "Topstep Quant Suite 12: kinetic_velocity", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(2.5, 2.0, 50, 0.5, 1.5, 6), init_quant_v<5, 0xF>);
    add_quant("topstep_quant_suite12_ldn_ny_reset", "Topstep Quant Suite 12: ldn_ny_reset", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0x4, q_schema(0.15, 1.5, 14, 0.25, 1.0, 2), init_quant_v<6, 0x4>);
    add_quant("topstep_quant_suite12_moc_drift", "Topstep Quant Suite 12: moc_drift", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0x8, q_schema(1.2, 1.0, 14, 0.4, 0.9, 1), init_quant_v<7, 0x8>);
    add_quant("topstep_quant_suite12_smt_divergence", "Topstep Quant Suite 12: smt_divergence", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "CROSS_ASSET_CAUSAL", true, 0xF, q_schema(0.10, 0.0, 15, 0.25, 1.0, 4), init_quant_v<8, 0xF>);
    add_quant("topstep_quant_suite12_turtle_soup", "Topstep Quant Suite 12: turtle_soup", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xC, q_schema(0.25, 1.5, 14, 0.25, 1.0, 2), init_quant_v<9, 0xC>);
    add_quant("topstep_quant_suite12_vsa_absorption", "Topstep Quant Suite 12: vsa_absorption", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(2.5, 0.6, 20, 0.15, 1.5, 3), init_quant_v<10, 0xF>);
    add_quant("topstep_quant_suite12_vwap_multi_anchor", "Topstep Quant Suite 12: vwap_multi_anchor", "10_Topstep_Quant_Suite_and_Standalones",
              "quant_suite.py", "EXACT_SOURCE", false, 0xF, q_schema(2.5, 3.2, 20, 1.5, 1.0, 3), init_quant_v<11, 0xF>);

    add_orb("topstep_standalone_crabel_orb_1to1", "Toby Crabel 1:1 Full Methodology ORB", "10_Topstep_Quant_Suite_and_Standalones",
            "crabel_orb_1to1.py", "EXACT_SOURCE", 0x4, orb_schema(15, 0.0, 20.0, 1.5, 0.75, 0.35, 1, 0, 1.0), init_orb_v<1>);
    add_quant("topstep_standalone_session_switch", "Session Switch (London -> NY Liquidity Zone)", "10_Topstep_Quant_Suite_and_Standalones",
              "session_switch.py", "EXACT_SOURCE", false, 0x4, q_schema(1.5, 2.0, 20, 1.0, 2.0, 1), init_quant_v<16, 0x4>);
    add_quant("vwap_fade", "Session-Anchored VWAP Fade Scalper", "10_Topstep_Quant_Suite_and_Standalones",
              "vwap_fade_sweep.py", "EXACT_SOURCE", false, 0xF, q_schema(15.0, 1.0, 20, 15.0, 15.0, 4), init_quant_v<17, 0xF>);
    add_quant("reels_engine_reel_27_model_d_pdh_pdl_breakout_expansion", "Reel 27 Model D: PDH/PDL Breakout Expansion", "10_Topstep_Quant_Suite_and_Standalones",
              "run_all_46_reels_validation.py", "EXACT_SOURCE", false, 0xC, q_schema(1.0, 1.0, 5, 20.0, 40.0, 2), init_quant_v<18, 0xC>);
    add_quant("qullamaggie_trading_system", "Kristjan Qullamaggie Holistic Swing Breakout", "10_Topstep_Quant_Suite_and_Standalones",
              "qullamaggie_trading_system", "EXACT_FORMULA", false, 0xC, q_schema(1.5, 1.0, 20, 1.5, 3.0, 2), init_quant_v<19, 0xC>);
    add_quant("fvg_execution_engine", "ICT Multi-Timeframe Fair Value Gap (FVG) Execution Engine", "10_Topstep_Quant_Suite_and_Standalones",
              "fair_value_gap.py", "EXACT_SOURCE", false, 0xF, q_schema(25.0, 1.0, 50, 5.0, 60.0, 4), init_quant_v<20, 0xF>);

    // ------------------------------------------------------------------------
    // 7. Kernel Experimental Sweep Mechanisms (8 UIDs)
    // ------------------------------------------------------------------------
    add_phys("exp_12cell_reentry_vwap_swing_ema", "12-Cell Decoupled Re-Entry Champions (VWAP/Swing15m/EMA21)",
             "13_Kernel_Experimental_Sweep_Mechanisms", "sweep_12cell_decoupled_reentries_v2.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 1.0, 12), init_physics_v<9>);
    add_phys("exp_amd_architecture", "Accumulation-Manipulation-Distribution (AMD / Judas Swing) Filter Sweep",
             "13_Kernel_Experimental_Sweep_Mechanisms", "sweep_amd_architecture.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 4.0, 12), init_physics_v<10>);
    add_sniper("exp_legacy_portfolio_stacks_paper1_paper2", "Legacy Multi-Strategy Portfolio Stacks (London1_NY1)",
               "13_Kernel_Experimental_Sweep_Mechanisms", "COMPLETE_STRATEGY_REGISTRY_AND_CONFIGURATIONS.md", "EXACT_FORMULA",
               0xE, u11::ParamSchema{{
                   p_lin("sl_pts", 15.0, 6.0, 30.0, 5), p_lin("rr_mult", 2.0, 1.0, 3.5, 5),
                   p_lin("be_trig_r", 0.50, 0.25, 1.0, 4), p_lin("be_offset_pts", 2.0, 0.5, 4.0, 4),
                   p_int("lookback", 15, 15, 30, 2), p_int("max_per_day", 3, 1, 5, 4)
               }}, init_sniper_v<10>);
    add_phys("exp_magnet_and_dip_reclaim_overlays", "Magnet, Dip-Reclaim, Asymmetric Runner & Scale-In-to-P0",
             "13_Kernel_Experimental_Sweep_Mechanisms", "sweep_dip_reclaim_brackets.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 12.0, 12), init_physics_v<11>);
    add_phys("exp_phase1_thursday_causal_suite", "Causal Bug-Immune Phase 1 Formula & $50-$950 Stop Sweep",
             "13_Kernel_Experimental_Sweep_Mechanisms", "phase1_thursday_formula_stop_sweep.cpp", "EXACT_SOURCE",
             0xF, u11::ParamSchema{{
                 p_lin("sl_pts", 10.0, 5.0, 40.0, 5),
                 p_lin("tp_pts", 60.0, 12.0, 100.0, 5),
                 p_lin("ratchet_trig_pts", 1.50, 0.25, 5.0, 5),
                 p_lin("ratchet_trail_pts", 0.50, 0.25, 3.0, 5),
                 p_lin("or_buf_pts", 1.0, 0.0, 5.0, 4),
                 p_lin("th_159", 1.50, 0.8, 3.0, 4),
                 p_lin("th_151", 0.80, 0.4, 1.0, 4),
                 p_lin("th_157", 3.00, 1.5, 5.0, 4),
                 p_disc("or_w", 30.0, {15.0, 30.0, 45.0, 60.0}),
                 p_int("max_entries", 12, 2, 20, 4)
             }}, init_physics_v<12>);
    add_phys("exp_social_alpha_trio_and_hybrids", "Social Alpha Trio Stack & Hybrid Velocity Ensemble",
             "13_Kernel_Experimental_Sweep_Mechanisms", "run_decoupled_session_snipers_tournament.py", "EXACT_SOURCE",
             0xF, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 2.0, 12), init_physics_v<13>);
    add_phys("exp_squeeze_confluences", "Bollinger Squeeze & 3-Way Confluence Sweep",
             "13_Kernel_Experimental_Sweep_Mechanisms", "sweep_squeeze_confluences.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 2.0, 12), init_physics_v<14>);
    add_phys("exp_staging_overlays_pyramiding_apex", "Top-40 Execution Overlays, Pyramiding & Granular Ratchet Sweeps",
             "13_Kernel_Experimental_Sweep_Mechanisms", "sweep_top40_execution_overlays.cpp", "EXACT_SOURCE",
             0x7, phys_schema(8.0, 60.0, 1.25, 0.50, 1.0, 1.50, 0.80, 3.00, 1.0, 12), init_physics_v<15>);
}

} // namespace s546
