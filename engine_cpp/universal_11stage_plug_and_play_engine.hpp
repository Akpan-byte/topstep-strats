#pragma once
// ============================================================================
// UNIVERSAL 11-STAGE PLUG-AND-PLAY CAUSAL VALIDATION ENGINE (C++17 / OpenMP)
// ============================================================================
// Zero .bin Trade Pre-Baking | Strict Bar-by-Bar Chronological Stepping
// CausalBarView Sandbox (lag > 0 throws std::out_of_range)
// Pessimistic Friction: 1.50pt In / 2.00pt Out + $1.40/Micro | $4.50/Mini
// Intrabar Wick SL & -$1,000 DLL Precedence Over TP | Interbar-Only Ratchets
// Overlapping 64-Chunk Parallelization with Bit-Exact Serial Equivalence
// Full 11-Stage SOP Pipeline (Unconstrained, H1/H2, PSR/DSR/Markov/Hurst/Bayes,
//   5M & 1M Rolling Combines, Funded Policy A/B, 30k Block MC, 30k Noise MC,
//   and 2M..50M Contract Scaling Sweep)
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace u11 {

#pragma pack(push, 1)
struct RawBar {
    int64_t ts_sec;
    float o, h, l, c, v;
    float pad1, pad2;
};

struct Bar48 {
    int64_t ts_sec;
    double o, h, l, c, v;
};
#pragma pack(pop)

static_assert(sizeof(RawBar) == 36, "RawBar must be 36 bytes");
static_assert(sizeof(Bar48) == 48, "Bar48 must be 48 bytes");

// ----------------------------------------------------------------------------
// Symbol-Agnostic InstrumentSpec Registry
// ----------------------------------------------------------------------------
struct InstrumentSpec {
    std::string symbol = "NQ";
    double micro_pv = 2.0;
    double mini_pv = 20.0;
    double tick_size = 0.25;
    double entry_slip_pts = 1.50;
    double exit_slip_pts = 2.00;
    double fee_rt_micro = 1.40;
    double fee_rt_mini = 4.50;
    double baseline_index = 4524.25;
};

inline InstrumentSpec get_instrument_spec(const std::string& sym) {
    if (sym == "NQ" || sym == "MNQ") {
        return {"NQ", 2.0, 20.0, 0.25, 1.50, 2.00, 1.40, 4.50, 4524.25};
    }
    if (sym == "ES" || sym == "MES") {
        return {"ES", 5.0, 50.0, 0.25, 1.50, 2.00, 1.40, 4.50, 2100.0};
    }
    if (sym == "YM" || sym == "MYM") {
        return {"YM", 0.50, 5.0, 1.00, 1.50, 2.00, 1.40, 4.50, 17777.0};
    }
    throw std::invalid_argument("Unknown InstrumentSpec symbol: " + sym);
}

struct BarMeta {
    int32_t cme_day_idx;   // 0 .. total_days - 1
    int16_t et_min;        // 0 .. 1439 (DST-aware America/New_York)
    int8_t  sess_id;       // 0=Asian, 1=London, 2=NY_AM, 3=NY_PM, -1=Outside
    int16_t min_in_sess;   // Minutes since session open
    bool    is_new_day;
    bool    is_new_sess;
};

struct CanonicalPercentiles {
    double p01 = 0.0, p05 = 0.0, p10 = 0.0, p20 = 0.0, p50 = 0.0;
    double p80 = 0.0, p90 = 0.0, p95 = 0.0, p99 = 0.0;
    double mean = 0.0, min_val = 0.0, max_val = 0.0, stdev = 0.0;
};

inline CanonicalPercentiles calc_percentiles(std::vector<double> v) {
    CanonicalPercentiles p{};
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    auto get_p = [&](double pct) -> double {
        size_t idx = std::min(static_cast<size_t>(pct * static_cast<double>(n)), n - 1);
        return v[idx];
    };
    p.p01 = get_p(0.01);
    p.p05 = get_p(0.05);
    p.p10 = get_p(0.10);
    p.p20 = get_p(0.20);
    p.p50 = get_p(0.50);
    p.p80 = get_p(0.80);
    p.p90 = get_p(0.90);
    p.p95 = get_p(0.95);
    p.p99 = get_p(0.99);

    double sum = 0.0;
    for (double x : v) sum += x;
    p.mean = sum / static_cast<double>(n);
    p.min_val = v.front();
    p.max_val = v.back();
    double var = 0.0;
    for (double x : v) {
        double d = x - p.mean;
        var += d * d;
    }
    p.stdev = std::sqrt(var / static_cast<double>(n));
    return p;
}

// ----------------------------------------------------------------------------
// DST-Aware US Eastern Time Helpers
// ----------------------------------------------------------------------------
inline bool is_us_dst_utc_day(int m, int d, int wday) {
    return (m > 3 && m < 11) || (m == 3 && (d - wday) >= 8) || (m == 11 && (d - wday) < 1);
}

inline int get_us_et_offset_sec(int64_t ts_sec) {
    time_t raw = static_cast<time_t>(ts_sec);
    struct tm t{};
    gmtime_r(&raw, &t);
    int m = t.tm_mon + 1;
    int d = t.tm_mday;
    int wday = t.tm_wday; // 0=Sun..6=Sat
    return is_us_dst_utc_day(m, d, wday) ? (-4 * 3600) : (-5 * 3600);
}

inline void decode_et_and_cme_date(int64_t ts_sec, int16_t& et_min_out, int32_t& cme_yyyymmdd_out) {
    int64_t et_sec = ts_sec + get_us_et_offset_sec(ts_sec);
    time_t et_raw = static_cast<time_t>(et_sec);
    struct tm et_t{};
    gmtime_r(&et_raw, &et_t);
    et_min_out = static_cast<int16_t>(et_t.tm_hour * 60 + et_t.tm_min);

    int64_t cme_sec = et_sec + 6 * 3600;
    time_t cme_raw = static_cast<time_t>(cme_sec);
    struct tm cme_t{};
    gmtime_r(&cme_raw, &cme_t);
    cme_yyyymmdd_out = (cme_t.tm_year + 1900) * 10000 + (cme_t.tm_mon + 1) * 100 + cme_t.tm_mday;
}

inline int8_t classify_session(int16_t et_min, bool include_ny_pm) {
    if (et_min >= 1080 || et_min < 120) return 0; // Asian: 18:00 - 02:00 ET
    if (et_min >= 180 && et_min < 360)  return 1; // London: 03:00 - 06:00 ET
    if (et_min >= 570 && et_min < 690)  return 2; // NY_AM: 09:30 - 11:30 ET
    if (include_ny_pm && et_min >= 810 && et_min < 960) return 3; // NY_PM: 13:30 - 16:00 ET
    return -1;
}

inline int16_t calc_min_in_sess(int16_t et_min, int8_t sess_id) {
    if (sess_id == 0) return (et_min >= 1080) ? static_cast<int16_t>(et_min - 1080) : static_cast<int16_t>((1440 - 1080) + et_min);
    if (sess_id == 1) return static_cast<int16_t>(et_min - 180);
    if (sess_id == 2) return static_cast<int16_t>(et_min - 570);
    if (sess_id == 3) return static_cast<int16_t>(et_min - 810);
    return 0;
}

inline double exact_calendar_years(int32_t start_yyyymmdd, int32_t end_yyyymmdd) {
    if (start_yyyymmdd <= 0 || end_yyyymmdd <= start_yyyymmdd) return 1.0;
    auto to_epoch_days = [](int32_t ymd) -> double {
        int y = ymd / 10000;
        int m = (ymd / 100) % 100;
        int d = ymd % 100;
        return static_cast<double>(y) * 365.2425 + static_cast<double>(m - 1) * 30.436875 + static_cast<double>(d);
    };
    return std::max(1.0 / 365.2425, (to_epoch_days(end_yyyymmdd) - to_epoch_days(start_yyyymmdd)) / 365.2425);
}

inline int64_t normalize_ts_sec(int64_t raw_ts) {
    if (raw_ts > 100000000000000LL) return raw_ts / 1000000000LL; // ns -> s
    if (raw_ts > 100000000000LL) return raw_ts / 1000LL;          // ms -> s
    return raw_ts;
}

// ----------------------------------------------------------------------------
// Extended PrecomputedMarketData (Strictly Causal Per-Bar Features)
// ----------------------------------------------------------------------------
struct PrecomputedMarketData {
    size_t n_bars = 0;
    int32_t total_days = 0;
    std::vector<RawBar> bars;
    std::vector<BarMeta> meta;
    std::vector<int32_t> day_yyyymmdd;
    std::vector<size_t> day_start_bar;
    std::vector<size_t> day_end_bar; // exclusive

    // Volatility & Trend
    std::vector<double> atr7, atr14, atr21, atr60;
    std::vector<double> ema9, ema20, ema50;

    // Volume (BUG-18 regime-invariant relative volume)
    std::vector<double> vol_ema20;
    std::vector<double> rel_vol;

    // Session-reset VWAP & StdDev
    std::vector<double> session_vwap;
    std::vector<double> session_vwap_std;

    // Prior CME Day High / Low / Close
    std::vector<double> pdh, pdl, pdc;
    std::vector<uint8_t> has_prior_day_flag;

    // Opening Ranges & Current-Session IB60
    std::vector<float> or15_h, or15_l;
    std::vector<float> or30_h, or30_l;
    std::vector<float> or45_h, or45_l;
    std::vector<float> or60_h, or60_l;
    std::vector<float> ib60_h, ib60_l, ib60_mid;
    std::vector<uint8_t> or_formed_mask; // bit0=15m, bit1=30m, bit2=45m, bit3=60m

    bool init_from_bars(std::vector<RawBar>&& raw_in, bool include_ny_pm = true,
                        int32_t start_yyyymmdd = 0, int32_t end_yyyymmdd = 99999999) {
        bars.clear();
        bars.reserve(raw_in.size());
        for (const auto& rb : raw_in) {
            if (rb.o <= 1000.0f || rb.h <= 1000.0f || rb.l <= 1000.0f || rb.c <= 1000.0f) continue;
            if (rb.h < rb.l) continue;
            int16_t tmp_em = 0; int32_t tmp_dt = 0;
            decode_et_and_cme_date(rb.ts_sec, tmp_em, tmp_dt);
            if (tmp_dt < start_yyyymmdd || tmp_dt > end_yyyymmdd) continue;
            bars.push_back(rb);
        }
        n_bars = bars.size();
        if (n_bars == 0) return false;

        meta.resize(n_bars);
        atr7.resize(n_bars);
        atr14.resize(n_bars);
        atr21.resize(n_bars);
        atr60.resize(n_bars);
        ema9.resize(n_bars);
        ema20.resize(n_bars);
        ema50.resize(n_bars);
        vol_ema20.resize(n_bars);
        rel_vol.resize(n_bars);
        session_vwap.assign(n_bars, 0.0);
        session_vwap_std.assign(n_bars, 0.0);
        pdh.assign(n_bars, 0.0);
        pdl.assign(n_bars, 0.0);
        pdc.assign(n_bars, 0.0);
        has_prior_day_flag.assign(n_bars, 0);

        or15_h.assign(n_bars, -1e9f); or15_l.assign(n_bars, 1e9f);
        or30_h.assign(n_bars, -1e9f); or30_l.assign(n_bars, 1e9f);
        or45_h.assign(n_bars, -1e9f); or45_l.assign(n_bars, 1e9f);
        or60_h.assign(n_bars, -1e9f); or60_l.assign(n_bars, 1e9f);
        ib60_h.assign(n_bars, -1e9f); ib60_l.assign(n_bars, 1e9f); ib60_mid.assign(n_bars, 0.0f);
        or_formed_mask.assign(n_bars, 0);

        day_yyyymmdd.clear();
        day_start_bar.clear();
        day_end_bar.clear();

        int32_t prev_cme_date = -1;
        int32_t cur_day_idx = -1;
        int8_t  prev_sess = -1;

        double a7 = 0.0, a14 = 0.0, a21 = 0.0;
        double tr_ring[60]{};
        double tr_sum60 = 0.0;
        double e9 = 0.0, e20 = 0.0, e50 = 0.0, ve20 = 0.0;
        constexpr double K9  = 2.0 / 10.0;
        constexpr double K20 = 2.0 / 21.0;
        constexpr double K50 = 2.0 / 51.0;

        // Prior CME Day tracking
        bool has_prev_day = false;
        double prev_dh = 0.0, prev_dl = 0.0, prev_dc = 0.0;
        double cur_dh = -1e18, cur_dl = 1e18, cur_dc = 0.0;

        // Session VWAP state (shifted Welford/sum for precision)
        int32_t last_vwap_key = -1;
        double vw_tp0 = 0.0, vw_sum_v = 0.0, vw_sum_dv = 0.0, vw_sum_d2v = 0.0;

        // Causal OR state per session
        double h15 = -1e9, l15 = 1e9; bool s15 = false, f15 = false;
        double h30 = -1e9, l30 = 1e9; bool s30 = false, f30 = false;
        double h45 = -1e9, l45 = 1e9; bool s45 = false, f45 = false;
        double h60 = -1e9, l60 = 1e9; bool s60 = false, f60 = false;
        int32_t last_or_key = -1;

        for (size_t i = 0; i < n_bars; ++i) {
            const auto& b = bars[i];
            int16_t et_min = 0;
            int32_t cme_dt = 0;
            decode_et_and_cme_date(b.ts_sec, et_min, cme_dt);

            bool new_day = (cme_dt != prev_cme_date);
            if (new_day) {
                if (cur_day_idx >= 0) {
                    day_end_bar.push_back(i);
                    prev_dh = cur_dh;
                    prev_dl = cur_dl;
                    prev_dc = cur_dc;
                    has_prev_day = true;
                }
                cur_day_idx++;
                day_yyyymmdd.push_back(cme_dt);
                day_start_bar.push_back(i);
                prev_cme_date = cme_dt;
                cur_dh = static_cast<double>(b.h);
                cur_dl = static_cast<double>(b.l);
                cur_dc = static_cast<double>(b.c);
            } else {
                cur_dh = std::max(cur_dh, static_cast<double>(b.h));
                cur_dl = std::min(cur_dl, static_cast<double>(b.l));
                cur_dc = static_cast<double>(b.c);
            }

            int8_t sess = classify_session(et_min, include_ny_pm);
            bool new_sess = (sess != prev_sess) || new_day;
            prev_sess = sess;

            meta[i].cme_day_idx = cur_day_idx;
            meta[i].et_min = et_min;
            meta[i].sess_id = sess;
            meta[i].min_in_sess = (sess >= 0) ? calc_min_in_sess(et_min, sess) : 0;
            meta[i].is_new_day = new_day;
            meta[i].is_new_sess = new_sess;

            // Prior CME Day OHLC (strictly from COMPLETED prior day)
            if (has_prev_day) {
                pdh[i] = prev_dh;
                pdl[i] = prev_dl;
                pdc[i] = prev_dc;
                has_prior_day_flag[i] = 1;
            }

            double bh = static_cast<double>(b.h);
            double bl = static_cast<double>(b.l);
            double bc = static_cast<double>(b.c);
            double bv = std::max(0.0, static_cast<double>(b.v));

            // Wilder ATR 7/14/21
            double tr = bh - bl;
            if (i > 0) {
                double pc = static_cast<double>(bars[i - 1].c);
                tr = std::max({tr, std::abs(bh - pc), std::abs(bl - pc)});
                a7  = (a7  * 6.0  + tr) / 7.0;
                a14 = (a14 * 13.0 + tr) / 14.0;
                a21 = (a21 * 20.0 + tr) / 21.0;

                e9  = K9  * bc + (1.0 - K9)  * e9;
                e20 = K20 * bc + (1.0 - K20) * e20;
                e50 = K50 * bc + (1.0 - K50) * e50;
                ve20 = K20 * bv + (1.0 - K20) * ve20;
            } else {
                a7 = a14 = a21 = tr;
                e9 = e20 = e50 = bc;
                ve20 = bv;
            }
            if (i < 60) {
                tr_ring[i] = tr;
                tr_sum60 += tr;
                atr60[i] = tr_sum60 / static_cast<double>(i + 1);
            } else {
                size_t slot = i % 60;
                tr_sum60 -= tr_ring[slot];
                tr_ring[slot] = tr;
                tr_sum60 += tr;
                atr60[i] = tr_sum60 / 60.0;
            }
            atr7[i] = a7; atr14[i] = a14; atr21[i] = a21;
            ema9[i] = e9; ema20[i] = e20; ema50[i] = e50;
            vol_ema20[i] = ve20;
            rel_vol[i] = bv / std::max(1.0, ve20);

            // Session-Reset VWAP & StdDev + Opening Range / IB60
            if (sess >= 0) {
                int32_t sess_key = (cur_day_idx << 3) | sess;
                double tp = (bh + bl + bc) / 3.0;
                double v_wt = (bv > 0.0) ? bv : 1.0;

                if (sess_key != last_vwap_key) {
                    last_vwap_key = sess_key;
                    vw_tp0 = tp;
                    vw_sum_v = v_wt;
                    vw_sum_dv = 0.0;
                    vw_sum_d2v = 0.0;
                } else {
                    double d = tp - vw_tp0;
                    vw_sum_v += v_wt;
                    vw_sum_dv += d * v_wt;
                    vw_sum_d2v += d * d * v_wt;
                }
                double mean_d = vw_sum_dv / vw_sum_v;
                double var = std::max(0.0, (vw_sum_d2v / vw_sum_v) - mean_d * mean_d);
                session_vwap[i] = vw_tp0 + mean_d;
                session_vwap_std[i] = std::sqrt(var);

                if (sess_key != last_or_key) {
                    last_or_key = sess_key;
                    h15 = -1e9; l15 = 1e9; s15 = false; f15 = false;
                    h30 = -1e9; l30 = 1e9; s30 = false; f30 = false;
                    h45 = -1e9; l45 = 1e9; s45 = false; f45 = false;
                    h60 = -1e9; l60 = 1e9; s60 = false; f60 = false;
                }
                int16_t mins = meta[i].min_in_sess;
                if (mins < 15) { h15 = std::max(h15, bh); l15 = std::min(l15, bl); s15 = true; }
                else if (s15) { f15 = true; }

                if (mins < 30) { h30 = std::max(h30, bh); l30 = std::min(l30, bl); s30 = true; }
                else if (s30) { f30 = true; }

                if (mins < 45) { h45 = std::max(h45, bh); l45 = std::min(l45, bl); s45 = true; }
                else if (s45) { f45 = true; }

                if (mins < 60) { h60 = std::max(h60, bh); l60 = std::min(l60, bl); s60 = true; }
                else if (s60) { f60 = true; }

                or15_h[i] = static_cast<float>(h15); or15_l[i] = static_cast<float>(l15);
                or30_h[i] = static_cast<float>(h30); or30_l[i] = static_cast<float>(l30);
                or45_h[i] = static_cast<float>(h45); or45_l[i] = static_cast<float>(l45);
                or60_h[i] = static_cast<float>(h60); or60_l[i] = static_cast<float>(l60);

                ib60_h[i] = or60_h[i];
                ib60_l[i] = or60_l[i];
                if (s60 && h60 > -1e8 && l60 < 1e8) {
                    ib60_mid[i] = static_cast<float>(0.5 * (h60 + l60));
                }

                uint8_t mask = 0;
                if (f15) mask |= 1;
                if (f30) mask |= 2;
                if (f45) mask |= 4;
                if (f60) mask |= 8;
                or_formed_mask[i] = mask;
            }
        }
        day_end_bar.push_back(n_bars);
        total_days = cur_day_idx + 1;
        return true;
    }

    bool load_binary_auto(const std::string& path, bool include_ny_pm = true,
                          int32_t start_yyyymmdd = 0, int32_t end_yyyymmdd = 99999999) {
        std::ifstream bf(path, std::ios::binary);
        if (!bf) return false;
        bf.seekg(0, std::ios::end);
        std::streamoff fsz_off = bf.tellg();
        if (fsz_off <= 0) return false;
        const size_t fsz = static_cast<size_t>(fsz_off);
        bf.seekg(0, std::ios::beg);

        const bool can36 = (fsz >= sizeof(RawBar)) && (fsz % sizeof(RawBar) == 0);
        const bool can48 = (fsz >= sizeof(Bar48)) && (fsz % sizeof(Bar48) == 0);
        if (!can36 && !can48) return false;

        constexpr size_t PROBE_BYTES = 16 * sizeof(Bar48);
        std::vector<char> probe(std::min(fsz, PROBE_BYTES));
        bf.read(probe.data(), static_cast<std::streamsize>(probe.size()));
        if (!bf) return false;

        auto is_plausible_ohlcv = [](int64_t raw_ts, double o, double h, double l, double c, double v, int64_t& prev_ts) -> bool {
            int64_t ts = normalize_ts_sec(raw_ts);
            if (ts < 946684800LL || ts > 4102444800LL) return false; // 2000-01-01..2100-01-01
            if (prev_ts > 0 && ts < prev_ts) return false;
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c) || !std::isfinite(v)) return false;
            if (o <= 100.0 || h <= 100.0 || l <= 100.0 || c <= 100.0) return false;
            if (o > 500000.0 || h > 500000.0 || l > 500000.0 || c > 500000.0) return false;
            if (h < l || h + 1e-6 < o || h + 1e-6 < c || l - 1e-6 > o || l - 1e-6 > c) return false;
            if (v < 0.0) return false;
            prev_ts = ts;
            return true;
        };

        bool valid36 = false;
        if (can36) {
            size_t n_chk = std::min<size_t>(12, probe.size() / sizeof(RawBar));
            valid36 = (n_chk > 0);
            int64_t prev_ts = 0;
            for (size_t i = 0; i < n_chk; ++i) {
                RawBar rb{};
                std::memcpy(&rb, probe.data() + i * sizeof(RawBar), sizeof(RawBar));
                if (!is_plausible_ohlcv(rb.ts_sec, rb.o, rb.h, rb.l, rb.c, rb.v, prev_ts)) {
                    valid36 = false;
                    break;
                }
            }
        }

        bool valid48 = false;
        if (can48) {
            size_t n_chk = std::min<size_t>(12, probe.size() / sizeof(Bar48));
            valid48 = (n_chk > 0);
            int64_t prev_ts = 0;
            for (size_t i = 0; i < n_chk; ++i) {
                Bar48 b48{};
                std::memcpy(&b48, probe.data() + i * sizeof(Bar48), sizeof(Bar48));
                if (!is_plausible_ohlcv(b48.ts_sec, b48.o, b48.h, b48.l, b48.c, b48.v, prev_ts)) {
                    valid48 = false;
                    break;
                }
            }
        }

        if (!valid36 && !valid48) return false;

        bf.seekg(0, std::ios::beg);
        if (valid48 && !valid36) {
            size_t nb = fsz / sizeof(Bar48);
            std::vector<Bar48> raw48(nb);
            bf.read(reinterpret_cast<char*>(raw48.data()), static_cast<std::streamsize>(nb * sizeof(Bar48)));
            bf.close();
            std::vector<RawBar> raw(nb);
            for (size_t i = 0; i < nb; ++i) {
                raw[i].ts_sec = normalize_ts_sec(raw48[i].ts_sec);
                raw[i].o = static_cast<float>(raw48[i].o);
                raw[i].h = static_cast<float>(raw48[i].h);
                raw[i].l = static_cast<float>(raw48[i].l);
                raw[i].c = static_cast<float>(raw48[i].c);
                raw[i].v = static_cast<float>(raw48[i].v);
                raw[i].pad1 = 0.0f;
                raw[i].pad2 = 0.0f;
            }
            return init_from_bars(std::move(raw), include_ny_pm, start_yyyymmdd, end_yyyymmdd);
        } else {
            size_t nb = fsz / sizeof(RawBar);
            std::vector<RawBar> raw(nb);
            bf.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(nb * sizeof(RawBar)));
            bf.close();
            for (size_t i = 0; i < nb; ++i) {
                raw[i].ts_sec = normalize_ts_sec(raw[i].ts_sec);
            }
            return init_from_bars(std::move(raw), include_ny_pm, start_yyyymmdd, end_yyyymmdd);
        }
    }

    bool load_binary_file(const std::string& path, bool include_ny_pm = true,
                          int32_t start_yyyymmdd = 0, int32_t end_yyyymmdd = 99999999) {
        return load_binary_auto(path, include_ny_pm, start_yyyymmdd, end_yyyymmdd);
    }
};

// ----------------------------------------------------------------------------
// CausalBarView Sandbox (Strict Lookahead Guard: lag > 0 throws!)
// ----------------------------------------------------------------------------
struct CausalBarView {
    const PrecomputedMarketData* md = nullptr;
    size_t curr_b = 0;
    bool in_position = false;
    int32_t sess_entries = 0;

    // Optional Strictly-Causal Cross-Asset Reference Feeds (ts_sec <= md->bars[curr_b].ts_sec)
    const PrecomputedMarketData* ref_nq = nullptr;
    const PrecomputedMarketData* ref_es = nullptr;
    const PrecomputedMarketData* ref_ym = nullptr;
    size_t ref_idx_nq = 0;
    size_t ref_idx_es = 0;
    size_t ref_idx_ym = 0;

    size_t causal_idx(int lag) const {
        if (lag > 0) {
            throw std::out_of_range("LOOKAHEAD_VIOLATION: lag > 0");
        }
        int64_t idx = static_cast<int64_t>(curr_b) + static_cast<int64_t>(lag);
        if (idx < 0) idx = 0;
        return static_cast<size_t>(idx);
    }

    static size_t ref_causal_idx(const PrecomputedMarketData* rmd, size_t base_idx, int lag) {
        if (lag > 0) {
            throw std::out_of_range("LOOKAHEAD_VIOLATION: ref lag > 0");
        }
        if (!rmd || rmd->n_bars == 0) return 0;
        int64_t idx = static_cast<int64_t>(base_idx) + static_cast<int64_t>(lag);
        if (idx < 0) idx = 0;
        size_t u = static_cast<size_t>(idx);
        return (u >= rmd->n_bars) ? (rmd->n_bars - 1) : u;
    }

    bool has_ref_nq() const { return ref_nq != nullptr && ref_nq->n_bars > 0; }
    bool has_ref_es() const { return ref_es != nullptr && ref_es->n_bars > 0; }
    bool has_ref_ym() const { return ref_ym != nullptr && ref_ym->n_bars > 0; }

    const RawBar& ref_nq_bar(int lag = 0) const {
        if (!has_ref_nq()) return (*this)[lag];
        return ref_nq->bars[ref_causal_idx(ref_nq, ref_idx_nq, lag)];
    }
    const RawBar& ref_es_bar(int lag = 0) const {
        if (!has_ref_es()) return (*this)[lag];
        return ref_es->bars[ref_causal_idx(ref_es, ref_idx_es, lag)];
    }
    const RawBar& ref_ym_bar(int lag = 0) const {
        if (!has_ref_ym()) return (*this)[lag];
        return ref_ym->bars[ref_causal_idx(ref_ym, ref_idx_ym, lag)];
    }
    double ref_nq_atr14(int lag = 0) const {
        if (!has_ref_nq()) return atr14(lag);
        return ref_nq->atr14[ref_causal_idx(ref_nq, ref_idx_nq, lag)];
    }
    double ref_es_atr14(int lag = 0) const {
        if (!has_ref_es()) return atr14(lag);
        return ref_es->atr14[ref_causal_idx(ref_es, ref_idx_es, lag)];
    }
    double ref_ym_atr14(int lag = 0) const {
        if (!has_ref_ym()) return atr14(lag);
        return ref_ym->atr14[ref_causal_idx(ref_ym, ref_idx_ym, lag)];
    }

    const RawBar& operator[](int lag) const {
        return md->bars[causal_idx(lag)];
    }

    double atr7(int lag = 0) const { return md->atr7[causal_idx(lag)]; }
    double atr14(int lag = 0) const { return md->atr14[causal_idx(lag)]; }
    double atr21(int lag = 0) const { return md->atr21[causal_idx(lag)]; }
    double atr60(int lag = 0) const { return md->atr60[causal_idx(lag)]; }
    double ema9(int lag = 0) const { return md->ema9[causal_idx(lag)]; }
    double ema20(int lag = 0) const { return md->ema20[causal_idx(lag)]; }
    double ema50(int lag = 0) const { return md->ema50[causal_idx(lag)]; }
    double session_vwap(int lag = 0) const { return md->session_vwap[causal_idx(lag)]; }
    double session_vwap_std(int lag = 0) const { return md->session_vwap_std[causal_idx(lag)]; }
    double rel_vol(int lag = 0) const { return md->rel_vol[causal_idx(lag)]; }

    bool has_prior_day() const { return md->has_prior_day_flag[curr_b] != 0; }
    double pdh() const { return md->pdh[curr_b]; }
    double pdl() const { return md->pdl[curr_b]; }
    double pdc() const { return md->pdc[curr_b]; }

    double ib60_h() const { return or_formed(60) ? static_cast<double>(md->ib60_h[curr_b]) : 0.0; }
    double ib60_l() const { return or_formed(60) ? static_cast<double>(md->ib60_l[curr_b]) : 0.0; }
    double ib60_mid() const { return or_formed(60) ? static_cast<double>(md->ib60_mid[curr_b]) : 0.0; }

    bool or_formed(int or_mins) const {
        uint8_t m = md->or_formed_mask[curr_b];
        if (or_mins == 15) return (m & 1) != 0;
        if (or_mins == 30) return (m & 2) != 0;
        if (or_mins == 45) return (m & 4) != 0;
        if (or_mins == 60) return (m & 8) != 0;
        return false;
    }

    double or_high(int or_mins) const {
        if (!or_formed(or_mins)) return 0.0;
        if (or_mins == 15) return static_cast<double>(md->or15_h[curr_b]);
        if (or_mins == 30) return static_cast<double>(md->or30_h[curr_b]);
        if (or_mins == 45) return static_cast<double>(md->or45_h[curr_b]);
        if (or_mins == 60) return static_cast<double>(md->or60_h[curr_b]);
        return 0.0;
    }

    double or_low(int or_mins) const {
        if (!or_formed(or_mins)) return 0.0;
        if (or_mins == 15) return static_cast<double>(md->or15_l[curr_b]);
        if (or_mins == 30) return static_cast<double>(md->or30_l[curr_b]);
        if (or_mins == 45) return static_cast<double>(md->or45_l[curr_b]);
        if (or_mins == 60) return static_cast<double>(md->or60_l[curr_b]);
        return 0.0;
    }

    int16_t et_mins() const { return md->meta[curr_b].et_min; }
    int8_t  session_id() const { return md->meta[curr_b].sess_id; }
    int16_t min_in_sess() const { return md->meta[curr_b].min_in_sess; }
    int32_t day_yyyymmdd() const { return md->day_yyyymmdd[md->meta[curr_b].cme_day_idx]; }
    int32_t cme_day_idx() const { return md->meta[curr_b].cme_day_idx; }
    size_t  bar_index() const { return curr_b; }
    bool    is_new_day() const { return md->meta[curr_b].is_new_day; }
    bool    is_new_sess() const { return md->meta[curr_b].is_new_sess; }
};

// ----------------------------------------------------------------------------
// Universal Plug-and-Play Strategy Signal Interface
// ----------------------------------------------------------------------------
struct PluginSignal {
    int8_t direction = 0;          // +1 Long, -1 Short, 0 Flat
    int8_t order_type = 0;         // 0 = Next-Bar Market Open, 1 = Next-Bar Resting Stop-Entry, 2 = Next-Bar Resting Limit-Entry
    double trigger_px = 0.0;       // Used when order_type == 1 (must trigger inside bar t+1 and add entry_slip_pts!)
    double sl_pts = 20.0;          // Initial hard stop distance in points (> 0)
    double tp_pts = 40.0;          // Target distance in points (0 = no fixed TP)
    double ratchet_trig_pts = 0.0; // Interbar trailing ratchet trigger in points (0 = disabled)
    double ratchet_trail_pts = 0.0;// Interbar trailing ratchet distance in points
    double be_trig_pts = 0.0;      // Interbar break-even trigger in points (0 = disabled)
    double be_offset_pts = 0.25;   // Break-even lock offset in points
    int32_t max_hold_bars = 45;    // Time stop in bars
    int32_t cooldown_bars = 1;     // Post-exit cooldown bars
};

struct StrategyPlugin {
    std::string name = "UnnamedPlugin";
    PluginSignal (*on_bar)(const CausalBarView& view, void* plugin_state) = nullptr;
    void* (*create_state)(void* user_ctx) = nullptr;
    void  (*destroy_state)(void* state) = nullptr;
    void* user_ctx = nullptr;
    std::shared_ptr<const void> ctx_owner = nullptr;
};

// Structural Side Geometry Validator (Gate 9)
inline bool validate_structural_side(int8_t direction, double stop_px, double target_px) {
    if (direction == 1)  return (target_px > stop_px);
    if (direction == -1) return (target_px < stop_px);
    return false;
}

inline double quantize_stop_tick(double px, int8_t pos_side, double tick_size) {
    if (pos_side == 1) return std::floor((px + 1e-6) / tick_size) * tick_size;
    return std::ceil((px - 1e-6) / tick_size) * tick_size;
}

// ----------------------------------------------------------------------------
// Engine Configuration & Records
// ----------------------------------------------------------------------------
struct EngineConfig {
    std::string name = "UNIVERSAL_11STAGE";
    InstrumentSpec inst = get_instrument_spec("NQ");
    int contracts_micros = 5;        // Default 5 Micros
    bool is_mini_mode = false;       // False: Micros (inst.micro_pv * contracts_micros, inst.fee_rt_micro); True: Minis ((inst.mini_pv/10) * contracts_micros, inst.fee_rt_mini)

    void set_instrument(const std::string& sym) { inst = get_instrument_spec(sym); }

    // Risk & Topstep 50k Combine Constraints
    int max_fleet_micros = 50;       // Hard <= 50M cap
    int max_concurrent_positions = 1;
    double sll_usd[4] = {0.0, 0.0, 0.0, 0.0}; // Session loss limits (<=0 disables SLL)
    double daily_dll_usd = 1000.0;
    double daily_profit_cap_usd = 1500.0;
    double mll_limit_usd = 2000.0;
    bool enforce_intrabar_mll = true;
    double target_profit_usd = 3000.0;
    double consistency_max_pct = 0.50001;
    int right_censor_buffer_days = 20;
    int max_trial_days = 180;
    bool allow_session[4] = {true, true, true, true};
    bool run_rolling_combines = true;

    // Parallel Chunking
    int warmup_days = 5;
    int num_chunks = 64;

    // 11-Stage SOP Controls (Full-2026 Quarantine: IS <= 20251231, H1 <= 20201231)
    int32_t h1_end_yyyymmdd = 20201231;
    int dsr_num_trials = 1;
    int mc_iterations = 30000;
    int mc_block_days = 5;
    uint64_t mc_seed = 20261119ULL;
    double noise_jitter_pts = 1.0;
    bool run_full_11_stages = true;
    bool assert_invariants = true;
    bool enforce_prop_limits_on_core = false; // False = Stage 3/5 TRULY unconstrained (no -$1k DLL / +$1.5k ProfCap clipping); Stages 6-8 & 11 ALWAYS enforce Topstep rules!

    // Optional Cross-Asset Causal References (nullptr if single-asset)
    const PrecomputedMarketData* ref_nq = nullptr;
    const PrecomputedMarketData* ref_es = nullptr;
    const PrecomputedMarketData* ref_ym = nullptr;
};

constexpr int32_t CANONICAL_IS_END_YYYYMMDD = 20251231;
constexpr int32_t CANONICAL_OOS_START_YYYYMMDD = 20260101;

inline double resolve_pv_per_micro_unit(const EngineConfig& cfg) {
    return cfg.is_mini_mode ? (cfg.inst.mini_pv / 10.0) : cfg.inst.micro_pv;
}

inline double resolve_rt_fee_usd(int micros, bool force_mini_mode, double fee_per_micro, double fee_per_mini) {
    if (force_mini_mode) {
        double minis = static_cast<double>(micros) / 10.0;
        return minis * fee_per_mini;
    }
    if (micros >= 10 && (micros % 10 == 0)) {
        return static_cast<double>(micros / 10) * fee_per_mini;
    }
    if (micros > 10) {
        int n_minis = micros / 10;
        int n_rem = micros % 10;
        return static_cast<double>(n_minis) * fee_per_mini + static_cast<double>(n_rem) * fee_per_micro;
    }
    return static_cast<double>(micros) * fee_per_micro;
}

inline void calc_ci95(std::vector<double> v, double& low_out, double& high_out) {
    if (v.empty()) { low_out = 0.0; high_out = 0.0; return; }
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    size_t i_lo = std::min(static_cast<size_t>(0.025 * static_cast<double>(n)), n - 1);
    size_t i_hi = std::min(static_cast<size_t>(0.975 * static_cast<double>(n)), n - 1);
    low_out = v[i_lo];
    high_out = v[i_hi];
}

struct TradeRecord {
    size_t  entry_bar = 0;
    size_t  exit_bar = 0;
    int32_t cme_day_idx = 0;
    int32_t cme_yyyymmdd = 0;
    int8_t  sess_id = 0;
    int8_t  side = 0;
    int     contracts_micros = 0;
    double  entry_px = 0.0;
    double  exit_px = 0.0;
    double  pts_net = 0.0;      // After 3.50pt round-trip slippage
    double  net_usd = 0.0;      // Configured size net USD (after slippage + fees)
    double  initial_risk_usd = 0.0; // Constant initial_sl R-denominator (Gate 8)
    double  r_multiple = 0.0;
    double  peak_mfe_pts = 0.0; // Peak favorable excursion from fill_px (pts)
    uint8_t exit_reason = 0;    // 0=SL/Trail, 1=TP, 2=Time, 3=SessEnd, 4=DLL, 5=SLL, 6=ProfitCap
};

struct BarEquitySample {
    size_t  bar_idx = 0;
    int32_t cme_day_idx = 0;
    double  realized_delta_in_bar = 0.0;
    double  worst_intra_delta_in_bar = 0.0;
    double  best_intra_delta_in_bar = 0.0;
};

struct DayRecord {
    int32_t cme_day_idx = 0;
    int32_t cme_yyyymmdd = 0;
    double  day_pnl = 0.0;
    double  worst_intra_delta = 0.0;
    bool    hit_dll = false;
    bool    consec_dll = false;
    int     trades_cnt = 0;
    double  sess_pnl[4]{};
    int8_t  dll_sess_id = -1;
};

struct CombineOutcome {
    int32_t start_day_idx = 0;
    int8_t  start_sess_id = 0;
    int8_t  status = 0; // 1=Pass, -1=Fail_MLL, -2=Fail_ConsecDLL, -3=Fail_Timeout
    int     days_elapsed = 0;
    double  max_intra_dd = 0.0;
    double  max_eod_dd = 0.0;
    double  trial_max_dd = 0.0;
    int     dll_hits = 0;
    double  final_pnl = 0.0;
};

struct CombineMetrics {
    int    total_trials = 0;
    int    pass_cnt = 0;
    int    fail_mll_cnt = 0;
    int    fail_consec_dll_cnt = 0;
    int    fail_timeout_cnt = 0;
    int    single_dll_days = 0;
    int    consec_dll_days = 0;
    double pass_rate_pct = 0.0;
    double day2_pass_pct = 0.0;
    double day3_pass_pct = 0.0;
    double day5_pass_pct = 0.0;
    double day10_pass_pct = 0.0;
    double day15_pass_pct = 0.0;
    double day20_pass_pct = 0.0;
    CanonicalPercentiles ttp;
    CanonicalPercentiles intra_dd;
    CanonicalPercentiles eod_dd;
    CanonicalPercentiles trial_max_dd;
    CanonicalPercentiles trial_dlls;
};

struct SplitMetrics {
    long long trades = 0;
    long long wins = 0;
    double win_rate_pct = 0.0;
    double profit_factor = 0.0;
    double net_pnl = 0.0;
    double curve_max_dd = 0.0;
    double sharpe_ann = 0.0;
};

struct TradeMicrostructureMetrics {
    double gross_profit_pts = 0.0;
    double gross_loss_pts = 0.0;
    double gross_profit_usd = 0.0;
    double gross_loss_usd = 0.0;
    double avg_win_pts = 0.0;
    double avg_loss_pts = 0.0;
    double avg_win_usd = 0.0;
    double avg_loss_usd = 0.0;
    double realized_payoff_ratio = 0.0;
    double expectancy_pts = 0.0;
    double expectancy_usd = 0.0;
    double max_win_pts = 0.0;
    double max_loss_pts = 0.0;
    double max_win_usd = 0.0;
    double max_loss_usd = 0.0;
    int    max_consec_wins = 0;
    int    max_consec_losses = 0;
    int    max_consec_win_days = 0;
    int    max_consec_loss_days = 0;
    double avg_bars_held_wins = 0.0;
    double avg_bars_held_losses = 0.0;
    double avg_bars_held_all = 0.0;
    double avg_mfe_giveback_pts = 0.0;
    long long exit_reason_counts[7]{};
    double    exit_reason_pct[7]{};
};

struct SessionQuantMetrics {
    long long trades = 0;
    long long wins = 0;
    double win_rate_pct = 0.0;
    double profit_factor = 0.0;
    double net_pts = 0.0;
    double net_usd = 0.0;
    double curve_max_dd = 0.0;
    double sharpe_ann = 0.0;
    double expectancy_pts = 0.0;
    int    dll_hits = 0;
};

struct HorizonWindows {
    SplitMetrics w_12m; // 252 CME days
    SplitMetrics w_6m;  // 126 CME days
    SplitMetrics w_4m;  // 84 CME days
    SplitMetrics w_3m;  // 63 CME days
    SplitMetrics w_2m;  // 42 CME days
    SplitMetrics w_1m;  // 21 CME days
    SplitMetrics w_2w;  // 10 CME days
    SplitMetrics w_1w;  // 5 CME days
    SplitMetrics ytd_latest_year;
    int32_t latest_year = 0;
};

struct StatisticalMetrics {
    double sharpe_ann = 0.0;
    double sortino_ann = 0.0;
    double calmar_ratio = 0.0;
    double psr = 0.0;          // Probabilistic Sharpe Ratio (0..1)
    double dsr = 0.0;          // Deflated Sharpe Ratio (0..1)
    double markov_p_ww = 0.0;  // P(Win | Prev Win)
    double markov_p_ll = 0.0;  // P(Loss | Prev Loss)
    double markov_clustering_bias = 0.0; // P(W|W) - (1 - P(L|L))
    double markov_p_dll_given_dll = 0.0; // P(DLL_{t+1} | DLL_t)
    double variance_ratio_5d = 1.0;
    double lo_mackinlay_vr2 = 1.0;
    double vr_z_stat = 0.0;
    double vr_p_value = 1.0;
    double hurst_exponent = 0.5;
    double bayes_wr_mean = 0.0;
    double bayes_wr_ci_low = 0.0;
    double bayes_wr_ci_high = 0.0;
    double ulcer_index_usd = 0.0;
    double ulcer_index_pct = 0.0;
    // Equity Curve Geometry & Tail Moments
    double r2_linear = 0.0;
    double r2_quadratic = 0.0;
    double quadratic_acceleration = 0.0;
    double underwater_pct = 0.0;
    int    max_underwater_days = 0;
    double max_dd_pct_of_pnl = 0.0;
    double return_skewness = 0.0;
    double return_kurtosis = 0.0; // Excess kurtosis (0.0 = Normal)
    double clean_sharpe = 0.0;
    double noise_sharpe = 0.0;
    double edge_retention_pct = 0.0;
};

struct FundedPolicyMetrics {
    int    cycles_tested = 0;
    int    payout_count = 0;
    int    blown_count = 0;
    double survival_rate_pct = 0.0;
    double total_payouts_usd = 0.0;
    double avg_payout_usd = 0.0;
    double expected_monthly_payout_usd = 0.0;
};

struct BootstrapMetrics {
    int    iterations = 0;
    double pass_rate_pct = 0.0;
    double ruin_rate_pct = 0.0;
    double profitable_pct = 0.0;
    double pass_2d_pct = 0.0;
    double pass_5d_pct = 0.0;
    double mean_ttp = 0.0;
    double ci95_sharpe_low = 0.0;
    double ci95_sharpe_high = 0.0;
    double ci95_pnl_low = 0.0;
    double ci95_pnl_high = 0.0;
    double ci95_wr_low = 0.0;
    double ci95_wr_high = 0.0;
    CanonicalPercentiles final_equity;
    CanonicalPercentiles max_dd;
};

struct ScalingTierMetrics {
    int    micros = 5;
    std::string label;
    double net_pnl = 0.0;
    double curve_max_dd = 0.0;
    double bar_curve_max_dd = 0.0;
    double combine_pass_rate_pct = 0.0;
    double combine_mll_fail_pct = 0.0;
    double median_ttp = 0.0;
    double p95_intra_dd = 0.0;
};

struct InstitutionalClassification {
    // 1. Outlier Dependency
    double net_pts_ex_top10 = 0.0;
    double pf_ex_top10 = 0.0;
    double top10_trades_pct_of_pnl = 0.0;
    int    positive_years_count = 0;
    int    total_years_count = 0;

    // 2. Directional Symmetry & 2022 Bear Survival
    SplitMetrics long_split;
    SplitMetrics short_split;
    SplitMetrics bear_2022_split;

    // 3. Breakeven Slippage Cushion
    double breakeven_slip_pts = 0.0;

    // 4. Portfolio Orthogonality & S&P 500 Buy-and-Hold Benchmark
    double corr_vs_benchmark = 0.0;
    double beta_vs_sp500 = 0.0;
    double jensen_alpha_ann_pct = 0.0;
    double ann_return_m_safe_pct = 0.0;
    bool   beats_sp500_sharpe = false;
    bool   beats_sp500_calmar = false;
    bool   beats_sp500_cagr_m_safe = false;
    bool   passes_stage3_sp500_screen = false;
    double sp500_cagr_pct = 0.0;
    double sp500_sharpe_ann = 0.0;
    double sp500_calmar_ratio = 0.0;
    double sp500_max_dd_pct = 0.0;

    // 5 & 6. Tier & Account-Suitability Badges
    std::string tier = "TIER_4_DISQUALIFIED_NO_CAUSAL_EDGE";
    std::string badges;
};

struct SP500Benchmark {
    std::vector<int32_t> yyyymmdd;
    std::vector<double> daily_ret;
    double cagr_pct = 0.0;
    double sharpe_ann = 0.0;
    double calmar_ratio = 0.0;
    double max_dd_pct = 0.0;
};

inline SP500Benchmark compute_sp500_benchmark(const PrecomputedMarketData& es_md) {
    SP500Benchmark sp{};
    if (es_md.total_days <= 0 || es_md.n_bars == 0) return sp;
    const size_t nd = static_cast<size_t>(es_md.total_days);
    sp.yyyymmdd = es_md.day_yyyymmdd;
    sp.daily_ret.resize(nd, 0.0);

    double prev_c = static_cast<double>(es_md.bars[es_md.day_start_bar[0]].o);
    double eq = 1.0, peak_eq = 1.0, max_dd = 0.0;
    for (size_t d = 0; d < nd; ++d) {
        size_t b_end_idx = es_md.day_end_bar[d];
        double c_d = static_cast<double>(es_md.bars[b_end_idx - 1].c);
        double ret = (prev_c > 1e-9) ? ((c_d - prev_c) / prev_c) : 0.0;
        sp.daily_ret[d] = ret;
        eq *= (1.0 + ret);
        if (eq > peak_eq) peak_eq = eq;
        double dd = (peak_eq > 1e-9) ? ((peak_eq - eq) / peak_eq) : 0.0;
        if (dd > max_dd) max_dd = dd;
        prev_c = c_d;
    }

    double years = exact_calendar_years(sp.yyyymmdd.front(), sp.yyyymmdd.back());
    sp.cagr_pct = (eq > 1e-9) ? ((std::pow(eq, 1.0 / years) - 1.0) * 100.0) : 0.0;
    sp.max_dd_pct = max_dd * 100.0;
    sp.calmar_ratio = (sp.max_dd_pct > 1e-9) ? (sp.cagr_pct / sp.max_dd_pct) : 0.0;

    if (nd >= 2) {
        double sum = 0.0;
        for (double r : sp.daily_ret) sum += r;
        double mean = sum / static_cast<double>(nd);
        double s2 = 0.0;
        for (double r : sp.daily_ret) {
            double diff = r - mean;
            s2 += diff * diff;
        }
        double sd = std::sqrt(s2 / static_cast<double>(nd - 1));
        double days_per_yr = static_cast<double>(nd) / years;
        sp.sharpe_ann = (sd > 1e-12) ? ((mean / sd) * std::sqrt(days_per_yr)) : 0.0;
    }
    return sp;
}

inline double compute_daily_pnl_correlation(const std::vector<DayRecord>& a, const std::vector<DayRecord>& b) {
    if (a.size() < 2 || b.size() < 2) return 0.0;
    std::vector<double> xa, xb;
    xa.reserve(std::min(a.size(), b.size()));
    xb.reserve(std::min(a.size(), b.size()));
    size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        if (a[ia].cme_yyyymmdd == b[ib].cme_yyyymmdd) {
            xa.push_back(a[ia].day_pnl);
            xb.push_back(b[ib].day_pnl);
            ++ia; ++ib;
        } else if (a[ia].cme_yyyymmdd < b[ib].cme_yyyymmdd) {
            ++ia;
        } else {
            ++ib;
        }
    }
    const size_t n = xa.size();
    if (n < 2) return 0.0;
    double ma = 0.0, mb = 0.0;
    for (size_t i = 0; i < n; ++i) { ma += xa[i]; mb += xb[i]; }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double num = 0.0, va = 0.0, vb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double da = xa[i] - ma;
        double db = xb[i] - mb;
        num += da * db;
        va += da * da;
        vb += db * db;
    }
    double denom = std::sqrt(va * vb);
    return (denom > 1e-12) ? (num / denom) : 0.0;
}

struct Universal11StageResult {
    std::string config_name;
    bool invariant_ok = true;
    size_t total_bars = 0;
    int32_t total_days = 0;
    double elapsed_ms = 0.0;

    // Stage 3: Configured + Points + Dual Granular (5 Micros & 1 Mini) + Auto M_safe
    long long total_trades = 0;
    long long winning_trades = 0;
    double win_rate_pct = 0.0;
    double profit_factor = 0.0;
    double trades_per_day = 0.0;
    double expectancy_r = 0.0;

    double net_pts = 0.0;
    double curve_max_dd_pts = 0.0;
    double net_pnl_configured = 0.0;
    double curve_max_dd_configured = 0.0;
    double bar_curve_max_dd_configured = 0.0;

    double net_pnl_5m = 0.0;
    double curve_max_dd_5m = 0.0;
    double bar_curve_max_dd_5m = 0.0;

    double net_pnl_1m = 0.0;
    double curve_max_dd_1m = 0.0;
    double bar_curve_max_dd_1m = 0.0;

    double p99_daily_intra_dd_per_micro = 0.0;
    int    m_safe_micros = 5;
    double net_pnl_m_safe = 0.0;
    double curve_max_dd_m_safe = 0.0;
    double bar_curve_max_dd_m_safe = 0.0;

    long long sess_trades[4]{};
    double    sess_pnl[4]{};
    SessionQuantMetrics sess_quant[4]{};

    // Trade Microstructure & Multi-Horizon Lookbacks
    TradeMicrostructureMetrics micro;
    HorizonWindows horizons;

    // Stage 4: H1 vs H2
    SplitMetrics h1;
    SplitMetrics h2;

    // Stage 5: Statistical Rigor
    StatisticalMetrics stats;

    // Stage 6 & 7: Prop-Firm Combines (Configured, 5 Micros, 1 Mini, M_safe)
    CombineMetrics combine_configured;
    CombineMetrics combine_5m;
    CombineMetrics combine_1m;
    CombineMetrics combine_m_safe;

    // Stage 8: Funded Policy A/B (Configured + M_safe)
    FundedPolicyMetrics policy_a;
    FundedPolicyMetrics policy_b;
    FundedPolicyMetrics policy_a_m_safe;
    FundedPolicyMetrics policy_b_m_safe;

    // Stage 9 & 10: 30k Block MC & 30k Noise MC (Configured + M_safe)
    BootstrapMetrics mc_block_30k;
    BootstrapMetrics mc_noise_30k;
    BootstrapMetrics mc_block_30k_m_safe;
    BootstrapMetrics mc_noise_30k_m_safe;

    // Stage 11: Scaling Sweep [2M, 5M, 10M, 15M, 20M, 30M, 50M]
    std::vector<ScalingTierMetrics> scaling_sweep;

    // Institutional Filters & Role Classification
    InstitutionalClassification inst;

    // Raw stitched stream (retained for parity/inspection)
    std::vector<TradeRecord> trades;
    std::vector<DayRecord> days;
};

// ----------------------------------------------------------------------------
// Internal Core Chunked Simulation Runner (Shared by Main + Combines + Scaling)
// ----------------------------------------------------------------------------
namespace detail {

struct ActivePos {
    int8_t  sess_id = 0;
    int8_t  side = 0;
    int     micros = 0;
    size_t  entry_bar = 0;
    double  entry_px = 0.0;
    double  stop_px = 0.0;
    double  target_px = 0.0;
    double  initial_risk_usd = 0.0;
    double  peak_mfe = 0.0;
    double  r_trig = 0.0;
    double  r_trail = 0.0;
    double  be_trig = 0.0;
    double  be_offset = 0.0;
    int32_t bars_held = 0;
    int32_t max_bars = 0;
    int32_t cooldown_bars = 1;
};

struct PendingOrder {
    int8_t  sess_id = 0;
    PluginSignal sig;
};

struct RollingTrial {
    int32_t start_day_idx = 0;
    int8_t  start_sess_id = 0;
    double  cum_pnl = 0.0;
    double  eod_peak = 0.0;
    double  intra_peak = 0.0;
    double  max_eod_dd = 0.0;
    double  max_intra_dd = 0.0;
    double  best_day_pnl = 0.0;
    int     days_elapsed = 1;
    int     dll_hits = 0;
    bool    prev_hit_dll = false;
    bool    stopped_for_day = false;
};

struct ChunkOutput {
    std::vector<TradeRecord> trades;
    std::vector<BarEquitySample> bar_samples;
    std::vector<DayRecord> days;
    std::vector<CombineOutcome> combines;
};

inline CombineMetrics aggregate_combine_outcomes(
    const std::vector<CombineOutcome>& outcomes,
    const std::vector<DayRecord>& days
) {
    CombineMetrics cm{};
    for (const auto& d : days) {
        if (d.hit_dll) cm.single_dll_days++;
        if (d.consec_dll) cm.consec_dll_days++;
    }
    if (outcomes.empty()) return cm;

    std::vector<double> v_ttp, v_intra, v_eod, v_maxdd, v_dlls;
    int le2 = 0, le3 = 0, le5 = 0, le10 = 0, le15 = 0, le20 = 0;
    for (const auto& o : outcomes) {
        cm.total_trials++;
        if (o.status == 1) {
            cm.pass_cnt++;
            v_ttp.push_back(static_cast<double>(o.days_elapsed));
            if (o.days_elapsed <= 2) le2++;
            if (o.days_elapsed <= 3) le3++;
            if (o.days_elapsed <= 5) le5++;
            if (o.days_elapsed <= 10) le10++;
            if (o.days_elapsed <= 15) le15++;
            if (o.days_elapsed <= 20) le20++;
        } else if (o.status == -1) cm.fail_mll_cnt++;
        else if (o.status == -2) cm.fail_consec_dll_cnt++;
        else if (o.status == -3) cm.fail_timeout_cnt++;

        v_intra.push_back(o.max_intra_dd);
        v_eod.push_back(o.max_eod_dd);
        v_maxdd.push_back(o.trial_max_dd);
        v_dlls.push_back(static_cast<double>(o.dll_hits));
    }
    const double n = static_cast<double>(cm.total_trials);
    cm.pass_rate_pct  = 100.0 * static_cast<double>(cm.pass_cnt) / n;
    cm.day2_pass_pct  = 100.0 * static_cast<double>(le2) / n;
    cm.day3_pass_pct  = 100.0 * static_cast<double>(le3) / n;
    cm.day5_pass_pct  = 100.0 * static_cast<double>(le5) / n;
    cm.day10_pass_pct = 100.0 * static_cast<double>(le10) / n;
    cm.day15_pass_pct = 100.0 * static_cast<double>(le15) / n;
    cm.day20_pass_pct = 100.0 * static_cast<double>(le20) / n;
    cm.ttp = calc_percentiles(v_ttp);
    cm.intra_dd = calc_percentiles(v_intra);
    cm.eod_dd = calc_percentiles(v_eod);
    cm.trial_max_dd = calc_percentiles(v_maxdd);
    cm.trial_dlls = calc_percentiles(v_dlls);
    return cm;
}

inline ChunkOutput simulate_chunk(
    const PrecomputedMarketData& md,
    const StrategyPlugin& plugin,
    const EngineConfig& cfg,
    int32_t d_start,
    int32_t d_end,
    bool collect_trades_and_days
) {
    ChunkOutput out;
    if (md.total_days <= 0 || d_start > d_end) return out;

    const int32_t w_start = std::max(0, d_start - std::max(0, cfg.warmup_days));
    const int32_t tail_end = cfg.run_rolling_combines
        ? std::min(md.total_days - 1, d_end + cfg.max_trial_days)
        : d_end;
    const int32_t max_start_day = std::max(0, md.total_days - cfg.right_censor_buffer_days);

    const size_t b_begin = md.day_start_bar[static_cast<size_t>(w_start)];
    const size_t b_end   = md.day_end_bar[static_cast<size_t>(tail_end)];

    const double pv = resolve_pv_per_micro_unit(cfg); // $ per point per 1 micro unit

    void* plugin_state = plugin.create_state ? plugin.create_state(plugin.user_ctx) : nullptr;

    std::vector<ActivePos> pool;
    pool.reserve(8);
    std::vector<PendingOrder> pending;
    pending.reserve(4);
    std::vector<RollingTrial> trials;
    trials.reserve(256);

    int32_t cur_day = -1;
    int8_t  cur_sess = -1;
    double  day_pnl = 0.0;
    double  day_worst_intra_delta = 0.0;
    bool    day_halted = false;
    bool    hit_dll_today = false;
    bool    prev_day_hit_dll = false;
    int8_t  dll_sess_id_today = -1;
    double  sess_pnl[4] = {0.0, 0.0, 0.0, 0.0};
    bool    sess_halted[4] = {false, false, false, false};

    int64_t last_exit_bar[4] = {-1, -1, -1, -1};
    int32_t active_cooldown[4] = {1, 1, 1, 1};
    int32_t sess_entries_cnt[4] = {0, 0, 0, 0};

    auto is_active_day = [&](int32_t d) -> bool {
        return d >= d_start && d <= d_end;
    };

    auto start_trials_for_sess = [&](int32_t d_idx, int8_t s_id) {
        if (!cfg.run_rolling_combines) return;
        if (!is_active_day(d_idx)) return;
        if (d_idx >= max_start_day) return;
        if (s_id < 0 || s_id > 2) return; // Official 3-start daily rolling protocol (Asian, London, NY_AM)
        RollingTrial rt;
        rt.start_day_idx = d_idx;
        rt.start_sess_id = s_id;
        rt.days_elapsed = 1;
        trials.push_back(rt);
    };

    auto apply_floating_dd_to_trials = [&](double worst_day_eq_delta) {
        if (!cfg.run_rolling_combines || trials.empty()) return;
        for (size_t k = 0; k < trials.size();) {
            auto& tr = trials[k];
            if (tr.stopped_for_day) { ++k; continue; }
            double cur_eq = tr.cum_pnl + worst_day_eq_delta;
            if (cur_eq > tr.intra_peak) tr.intra_peak = cur_eq;
            double dd = tr.intra_peak - cur_eq;
            if (dd > tr.max_intra_dd) tr.max_intra_dd = dd;

            bool mll_breached = cfg.enforce_intrabar_mll
                ? (tr.max_intra_dd >= cfg.mll_limit_usd || cur_eq <= -cfg.mll_limit_usd)
                : (cur_eq <= -cfg.mll_limit_usd);
            if (mll_breached) {
                out.combines.push_back({tr.start_day_idx, tr.start_sess_id, -1, tr.days_elapsed,
                    tr.max_intra_dd, tr.max_eod_dd, std::max(tr.max_intra_dd, tr.max_eod_dd), tr.dll_hits, cur_eq});
                trials[k] = trials.back();
                trials.pop_back();
                continue;
            }
            ++k;
        }
    };

    auto check_intraday_pass_on_trials = [&](double cur_day_pnl) {
        if (!cfg.run_rolling_combines || trials.empty()) return;
        for (size_t k = 0; k < trials.size();) {
            auto& tr = trials[k];
            if (tr.stopped_for_day) { ++k; continue; }
            double cur_eq = tr.cum_pnl + cur_day_pnl;
            if (cur_eq > tr.intra_peak) tr.intra_peak = cur_eq;
            double hypo_best = std::max(tr.best_day_pnl, cur_day_pnl);
            if (cur_eq >= cfg.target_profit_usd && tr.days_elapsed >= 2 && hypo_best <= cfg.consistency_max_pct * cur_eq) {
                out.combines.push_back({tr.start_day_idx, tr.start_sess_id, 1, tr.days_elapsed,
                    tr.max_intra_dd, tr.max_eod_dd, std::max(tr.max_intra_dd, tr.max_eod_dd), tr.dll_hits, cur_eq});
                trials[k] = trials.back();
                trials.pop_back();
                continue;
            }
            ++k;
        }
    };

    auto finalize_day_on_trials = [&](double final_day_pnl, bool dll_hit) {
        if (!cfg.run_rolling_combines || trials.empty()) return;
        for (size_t k = 0; k < trials.size();) {
            auto& tr = trials[k];
            tr.stopped_for_day = false;
            if (dll_hit) tr.dll_hits++;
            if (dll_hit && tr.prev_hit_dll) {
                out.combines.push_back({tr.start_day_idx, tr.start_sess_id, -2, tr.days_elapsed,
                    tr.max_intra_dd, tr.max_eod_dd, std::max(tr.max_intra_dd, tr.max_eod_dd), tr.dll_hits, tr.cum_pnl});
                trials[k] = trials.back();
                trials.pop_back();
                continue;
            }
            tr.prev_hit_dll = dll_hit;
            tr.cum_pnl += final_day_pnl;
            if (final_day_pnl > tr.best_day_pnl) tr.best_day_pnl = final_day_pnl;
            if (tr.cum_pnl > tr.eod_peak) tr.eod_peak = tr.cum_pnl;
            double eod_dd = tr.eod_peak - tr.cum_pnl;
            if (eod_dd > tr.max_eod_dd) tr.max_eod_dd = eod_dd;
            if (tr.cum_pnl > tr.intra_peak) tr.intra_peak = tr.cum_pnl;
            double intra_dd = tr.intra_peak - tr.cum_pnl;
            if (intra_dd > tr.max_intra_dd) tr.max_intra_dd = intra_dd;

            bool mll_hit = (tr.max_eod_dd >= cfg.mll_limit_usd || tr.cum_pnl <= -cfg.mll_limit_usd ||
                          (cfg.enforce_intrabar_mll && tr.max_intra_dd >= cfg.mll_limit_usd));
            if (mll_hit) {
                out.combines.push_back({tr.start_day_idx, tr.start_sess_id, -1, tr.days_elapsed,
                    tr.max_intra_dd, tr.max_eod_dd, std::max(tr.max_intra_dd, tr.max_eod_dd), tr.dll_hits, tr.cum_pnl});
                trials[k] = trials.back();
                trials.pop_back();
                continue;
            }
            if (tr.cum_pnl >= cfg.target_profit_usd && tr.days_elapsed >= 2 && tr.best_day_pnl <= cfg.consistency_max_pct * tr.cum_pnl) {
                out.combines.push_back({tr.start_day_idx, tr.start_sess_id, 1, tr.days_elapsed,
                    tr.max_intra_dd, tr.max_eod_dd, std::max(tr.max_intra_dd, tr.max_eod_dd), tr.dll_hits, tr.cum_pnl});
                trials[k] = trials.back();
                trials.pop_back();
                continue;
            }
            if (tr.days_elapsed >= cfg.max_trial_days) {
                out.combines.push_back({tr.start_day_idx, tr.start_sess_id, -3, tr.days_elapsed,
                    tr.max_intra_dd, tr.max_eod_dd, std::max(tr.max_intra_dd, tr.max_eod_dd), tr.dll_hits, tr.cum_pnl});
                trials[k] = trials.back();
                trials.pop_back();
                continue;
            }
            tr.days_elapsed++;
            ++k;
        }
    };

    int day_trades_cnt = 0;

    auto realize_trade = [&](const ActivePos& pos, double exit_px, size_t bar_idx, uint8_t reason) {
        double pts = (pos.side == 1) ? (exit_px - pos.entry_px) : (pos.entry_px - exit_px);
        double pos_fee = resolve_rt_fee_usd(pos.micros, cfg.is_mini_mode, cfg.inst.fee_rt_micro, cfg.inst.fee_rt_mini);
        double usd = (pts * pv * pos.micros) - pos_fee;
        day_pnl += usd;
        sess_pnl[pos.sess_id] += usd;
        day_trades_cnt++;
        last_exit_bar[pos.sess_id] = static_cast<int64_t>(bar_idx);
        active_cooldown[pos.sess_id] = pos.cooldown_bars;

        if (collect_trades_and_days && is_active_day(cur_day)) {
            TradeRecord tr{};
            tr.entry_bar = pos.entry_bar;
            tr.exit_bar = bar_idx;
            tr.cme_day_idx = cur_day;
            tr.cme_yyyymmdd = md.day_yyyymmdd[static_cast<size_t>(cur_day)];
            tr.sess_id = pos.sess_id;
            tr.side = pos.side;
            tr.contracts_micros = pos.micros;
            tr.entry_px = pos.entry_px;
            tr.exit_px = exit_px;
            tr.pts_net = pts;
            tr.net_usd = usd;
            tr.initial_risk_usd = pos.initial_risk_usd;
            tr.r_multiple = (pos.initial_risk_usd > 0.0) ? (usd / pos.initial_risk_usd) : 0.0;
            tr.peak_mfe_pts = std::max(0.0, std::max(pos.peak_mfe, pts));
            tr.exit_reason = reason;
            out.trades.push_back(tr);
        }
        return usd;
    };

    auto flatten_all_at_price = [&](double market_px, size_t bar_idx, uint8_t reason) {
        for (const auto& pos : pool) {
            double exit_px = (pos.side == 1) ? (market_px - cfg.inst.exit_slip_pts) : (market_px + cfg.inst.exit_slip_pts);
            realize_trade(pos, exit_px, bar_idx, reason);
        }
        pool.clear();
    };

    size_t ref_idx_nq = 0;
    size_t ref_idx_es = 0;
    size_t ref_idx_ym = 0;
    auto init_ref_cursor = [&](const PrecomputedMarketData* rmd, int64_t target_ts) -> size_t {
        if (!rmd || rmd->bars.empty()) return 0;
        const auto& rb = rmd->bars;
        auto it = std::upper_bound(rb.begin(), rb.end(), target_ts,
            [](int64_t ts, const RawBar& b) { return ts < b.ts_sec; });
        if (it == rb.begin()) return 0;
        return static_cast<size_t>((it - rb.begin()) - 1);
    };
    if (b_begin < b_end) {
        int64_t t0_ts = md.bars[b_begin].ts_sec;
        ref_idx_nq = init_ref_cursor(cfg.ref_nq, t0_ts);
        ref_idx_es = init_ref_cursor(cfg.ref_es, t0_ts);
        ref_idx_ym = init_ref_cursor(cfg.ref_ym, t0_ts);
    }

    for (size_t b = b_begin; b < b_end; ++b) {
        const auto& bar = md.bars[b];
        const auto& meta = md.meta[b];
        if (cfg.ref_nq && !cfg.ref_nq->bars.empty()) {
            while (ref_idx_nq + 1 < cfg.ref_nq->n_bars && cfg.ref_nq->bars[ref_idx_nq + 1].ts_sec <= bar.ts_sec) {
                ++ref_idx_nq;
            }
        }
        if (cfg.ref_es && !cfg.ref_es->bars.empty()) {
            while (ref_idx_es + 1 < cfg.ref_es->n_bars && cfg.ref_es->bars[ref_idx_es + 1].ts_sec <= bar.ts_sec) {
                ++ref_idx_es;
            }
        }
        if (cfg.ref_ym && !cfg.ref_ym->bars.empty()) {
            while (ref_idx_ym + 1 < cfg.ref_ym->n_bars && cfg.ref_ym->bars[ref_idx_ym + 1].ts_sec <= bar.ts_sec) {
                ++ref_idx_ym;
            }
        }

        // 1. Session & CME Day Transitions
        if (meta.is_new_sess || meta.is_new_day) {
            if (!pool.empty() && b > b_begin) {
                double before_flat = day_pnl;
                flatten_all_at_price(static_cast<double>(md.bars[b - 1].c), b - 1, 3);
                if (collect_trades_and_days && is_active_day(cur_day)) {
                    double delta = day_pnl - before_flat;
                    out.bar_samples.push_back({b - 1, cur_day, delta, std::min(0.0, delta), std::max(0.0, delta)});
                }
            }
            pending.clear();
        }
        if (meta.is_new_day) {
            if (cur_day >= 0) {
                bool consec = hit_dll_today && prev_day_hit_dll;
                if (collect_trades_and_days && is_active_day(cur_day)) {
                    DayRecord dr{};
                    dr.cme_day_idx = cur_day;
                    dr.cme_yyyymmdd = md.day_yyyymmdd[static_cast<size_t>(cur_day)];
                    dr.day_pnl = day_pnl;
                    dr.worst_intra_delta = day_worst_intra_delta;
                    dr.hit_dll = hit_dll_today;
                    dr.consec_dll = consec;
                    dr.trades_cnt = day_trades_cnt;
                    for (int s = 0; s < 4; ++s) dr.sess_pnl[s] = sess_pnl[s];
                    dr.dll_sess_id = dll_sess_id_today;
                    out.days.push_back(dr);
                }
                finalize_day_on_trials(day_pnl, hit_dll_today);
                prev_day_hit_dll = hit_dll_today;
            }
            // Tail early-exit optimization: once past d_end and prior day flushed, stop if all active trials finished
            if (meta.cme_day_idx > d_end && trials.empty()) {
                cur_day = -1;
                break;
            }
            cur_day = meta.cme_day_idx;
            day_pnl = 0.0;
            day_worst_intra_delta = 0.0;
            day_halted = false;
            hit_dll_today = false;
            dll_sess_id_today = -1;
            day_trades_cnt = 0;
            for (int s = 0; s < 4; ++s) { sess_pnl[s] = 0.0; sess_halted[s] = false; sess_entries_cnt[s] = 0; }
        }
        if (meta.is_new_sess) {
            cur_sess = meta.sess_id;
            if (cur_sess >= 0) start_trials_for_sess(cur_day, cur_sess);
        }

        if (meta.sess_id < 0 || !cfg.allow_session[meta.sess_id] || day_halted) {
            pending.clear();
            if (!pool.empty()) {
                flatten_all_at_price(static_cast<double>(bar.o), b, 3);
            }
            continue;
        }

        const int8_t s_id = meta.sess_id;
        const double bar_o = static_cast<double>(bar.o);
        const double bar_h = static_cast<double>(bar.h);
        const double bar_l = static_cast<double>(bar.l);
        const double bar_c = static_cast<double>(bar.c);
        const double day_pnl_before_bar = day_pnl;

        // 2. Execute Pending Entries queued from Bar b-1
        if (!pending.empty()) {
            for (const auto& po : pending) {
                if (day_halted || sess_halted[po.sess_id]) continue;
                if (cfg.max_concurrent_positions > 0 && static_cast<int>(pool.size()) >= cfg.max_concurrent_positions) continue;

                int eff_micros = cfg.contracts_micros;
                if (eff_micros <= 0) continue;
                int active_micros = 0;
                for (const auto& ap : pool) active_micros += ap.micros;
                if (active_micros + eff_micros > cfg.max_fleet_micros) continue;

                double ref_px = bar_o;
                if (po.sig.order_type == 1) {
                    // Resting Stop-Entry at trigger_px on bar b (marketable trigger -> pays entry_slip_pts)
                    if (po.sig.direction == 1) {
                        if (bar_h < po.sig.trigger_px) continue;
                        ref_px = std::max(bar_o, po.sig.trigger_px);
                    } else {
                        if (bar_l > po.sig.trigger_px) continue;
                        ref_px = std::min(bar_o, po.sig.trigger_px);
                    }
                } else if (po.sig.order_type == 2) {
                    // Price-touch marketable pullback trigger -> STILL pays full pessimistic entry_slip_pts (1.50pt)!
                    if (po.sig.direction == 1) {
                        if (bar_l > po.sig.trigger_px) continue;
                        ref_px = std::min(bar_o, po.sig.trigger_px);
                    } else {
                        if (bar_h < po.sig.trigger_px) continue;
                        ref_px = std::max(bar_o, po.sig.trigger_px);
                    }
                }

                double fill_px = (po.sig.direction == 1) ? (ref_px + cfg.inst.entry_slip_pts) : (ref_px - cfg.inst.entry_slip_pts);
                double init_stop = (po.sig.direction == 1) ? (fill_px - po.sig.sl_pts) : (fill_px + po.sig.sl_pts);
                init_stop = quantize_stop_tick(init_stop, po.sig.direction, cfg.inst.tick_size);
                double init_tp = 0.0;
                if (po.sig.tp_pts > 0.0) {
                    init_tp = (po.sig.direction == 1) ? (fill_px + po.sig.tp_pts) : (fill_px - po.sig.tp_pts);
                    if (!validate_structural_side(po.sig.direction, init_stop, init_tp)) continue;
                }

                double pos_fee = resolve_rt_fee_usd(eff_micros, cfg.is_mini_mode, cfg.inst.fee_rt_micro, cfg.inst.fee_rt_mini);
                double init_risk = (po.sig.sl_pts + cfg.inst.entry_slip_pts + cfg.inst.exit_slip_pts) * pv * eff_micros + pos_fee;

                ActivePos ap{};
                ap.sess_id = po.sess_id;
                ap.side = po.sig.direction;
                ap.micros = eff_micros;
                ap.entry_bar = b;
                ap.entry_px = fill_px;
                ap.stop_px = init_stop;
                ap.target_px = init_tp;
                ap.initial_risk_usd = init_risk;
                ap.peak_mfe = 0.0;
                ap.r_trig = po.sig.ratchet_trig_pts;
                ap.r_trail = po.sig.ratchet_trail_pts;
                ap.be_trig = po.sig.be_trig_pts;
                ap.be_offset = po.sig.be_offset_pts;
                ap.bars_held = 0;
                ap.max_bars = po.sig.max_hold_bars;
                ap.cooldown_bars = po.sig.cooldown_bars;
                pool.push_back(ap);
                sess_entries_cnt[po.sess_id]++;
            }
            pending.clear();
        }

        // 3. Evaluate Active Pool on Bar b (Intrabar Wick SL & DLL FIRST, TP strictly AFTER!)
        if (!pool.empty()) {
            double min_realized_day_pnl_in_bar = day_pnl;
            double max_realized_day_pnl_in_bar = day_pnl;

            // 3A. Intrabar Protective Stop FIRST (Strictly before DLL & TP!)
            for (size_t p_idx = 0; p_idx < pool.size();) {
                auto& pos = pool[p_idx];
                pos.bars_held++;
                bool hit_stop = false;
                double stop_fill_ref = pos.stop_px;

                if (pos.side == 1) {
                    if (bar_l <= pos.stop_px) {
                        hit_stop = true;
                        stop_fill_ref = (bar_o < pos.stop_px) ? bar_o : pos.stop_px;
                    }
                } else {
                    if (bar_h >= pos.stop_px) {
                        hit_stop = true;
                        stop_fill_ref = (bar_o > pos.stop_px) ? bar_o : pos.stop_px;
                    }
                }

                if (hit_stop) {
                    double exit_px = (pos.side == 1) ? (stop_fill_ref - cfg.inst.exit_slip_pts) : (stop_fill_ref + cfg.inst.exit_slip_pts);
                    realize_trade(pos, exit_px, b, 0);
                    if (day_pnl < min_realized_day_pnl_in_bar) min_realized_day_pnl_in_bar = day_pnl;
                    if (day_pnl > max_realized_day_pnl_in_bar) max_realized_day_pnl_in_bar = day_pnl;
                    pool[p_idx] = pool.back();
                    pool.pop_back();
                    continue;
                }
                ++p_idx;
            }

            // 3B. Mid-Candle Intrabar Floating DLL & Trailing MLL Check (Strictly before TP!)
            double worst_unr = 0.0;
            double best_unr = 0.0;
            if (!pool.empty()) {
                auto eval_unr_at_px = [&](double mkt_px) -> double {
                    double unr = 0.0;
                    for (const auto& pos : pool) {
                        double ex_px = (pos.side == 1) ? (mkt_px - cfg.inst.exit_slip_pts) : (mkt_px + cfg.inst.exit_slip_pts);
                        double pts = (pos.side == 1) ? (ex_px - pos.entry_px) : (pos.entry_px - ex_px);
                        double pos_fee = resolve_rt_fee_usd(pos.micros, cfg.is_mini_mode, cfg.inst.fee_rt_micro, cfg.inst.fee_rt_mini);
                        unr += (pts * pv * pos.micros) - pos_fee;
                    }
                    return unr;
                };
                double unr_low = eval_unr_at_px(bar_l);
                double unr_high = eval_unr_at_px(bar_h);
                worst_unr = std::min(unr_low, unr_high);
                best_unr = std::max(unr_low, unr_high);
            }
            double worst_day_eq = std::min(min_realized_day_pnl_in_bar, day_pnl + worst_unr);
            double best_day_eq  = std::max(max_realized_day_pnl_in_bar, day_pnl + best_unr);
            if (worst_day_eq < day_worst_intra_delta) day_worst_intra_delta = worst_day_eq;

            apply_floating_dd_to_trials(worst_day_eq);

            if (cfg.enforce_prop_limits_on_core && (worst_day_eq <= -cfg.daily_dll_usd || day_pnl <= -cfg.daily_dll_usd)) {
                if (!pool.empty()) {
                    // Instant Broker Intrabar DLL Liquidation capped at -daily_dll_usd
                    double rem_needed = (-cfg.daily_dll_usd) - day_pnl;
                    size_t n_rem = pool.size();
                    for (size_t k = 0; k < n_rem; ++k) {
                        const auto& pos = pool[k];
                        double target_usd = rem_needed / static_cast<double>(n_rem);
                        double pos_fee = resolve_rt_fee_usd(pos.micros, cfg.is_mini_mode, cfg.inst.fee_rt_micro, cfg.inst.fee_rt_mini);
                        double pts_needed = (target_usd + pos_fee) / (pv * pos.micros);
                        double liq_px = (pos.side == 1) ? (pos.entry_px + pts_needed) : (pos.entry_px - pts_needed);
                        realize_trade(pos, liq_px, b, 4);
                    }
                    pool.clear();
                }
                day_pnl = std::min(day_pnl, -cfg.daily_dll_usd);
                hit_dll_today = true;
                dll_sess_id_today = s_id;
                day_halted = true;
                if (collect_trades_and_days && is_active_day(cur_day)) {
                    double capped_worst = std::max(-cfg.daily_dll_usd, worst_day_eq);
                    out.bar_samples.push_back({b, cur_day, day_pnl - day_pnl_before_bar,
                        capped_worst - day_pnl_before_bar, best_day_eq - day_pnl_before_bar});
                }
                continue;
            }

            // 3C. Intrabar Profit Target Check (Strictly AFTER surviving SL & DLL wicks!)
            for (size_t p_idx = 0; p_idx < pool.size();) {
                auto& pos = pool[p_idx];
                if (pos.target_px > 0.0) {
                    bool hit_tp = (pos.side == 1) ? (bar_h >= pos.target_px) : (bar_l <= pos.target_px);
                    if (hit_tp) {
                        double tp_fav = (pos.side == 1) ? (pos.target_px - pos.entry_px) : (pos.entry_px - pos.target_px);
                        if (tp_fav > pos.peak_mfe) pos.peak_mfe = tp_fav;
                        double exit_px = (pos.side == 1) ? (pos.target_px - cfg.inst.exit_slip_pts) : (pos.target_px + cfg.inst.exit_slip_pts);
                        realize_trade(pos, exit_px, b, 1);
                        if (day_pnl > best_day_eq) best_day_eq = day_pnl;
                        pool[p_idx] = pool.back();
                        pool.pop_back();
                        continue;
                    }
                }
                ++p_idx;
            }

            // Un-clamped Session Loss Limit (SLL)
            if (cfg.enforce_prop_limits_on_core && cfg.sll_usd[s_id] > 0.0 && sess_pnl[s_id] <= -cfg.sll_usd[s_id]) {
                sess_halted[s_id] = true;
                for (size_t p_idx = 0; p_idx < pool.size();) {
                    if (pool[p_idx].sess_id == s_id) {
                        double exit_px = (pool[p_idx].side == 1) ? (bar_c - cfg.inst.exit_slip_pts) : (bar_c + cfg.inst.exit_slip_pts);
                        realize_trade(pool[p_idx], exit_px, b, 5);
                        pool[p_idx] = pool.back();
                        pool.pop_back();
                    } else {
                        ++p_idx;
                    }
                }
            }

            // Daily Profit Cap (+$1,500)
            if (cfg.enforce_prop_limits_on_core && day_pnl >= cfg.daily_profit_cap_usd) {
                flatten_all_at_price(bar_c, b, 6);
                day_halted = true;
                check_intraday_pass_on_trials(day_pnl);
                if (collect_trades_and_days && is_active_day(cur_day)) {
                    out.bar_samples.push_back({b, cur_day, day_pnl - day_pnl_before_bar,
                        std::min(day_pnl, worst_day_eq) - day_pnl_before_bar,
                        std::max(day_pnl, best_day_eq) - day_pnl_before_bar});
                }
                continue;
            }

            // 3C. Time-Stop Exits & INTERBAR-ONLY Break-Even + Trailing Ratchet at Bar b Close
            for (size_t p_idx = 0; p_idx < pool.size();) {
                auto& pos = pool[p_idx];
                if (pos.max_bars > 0 && pos.bars_held >= pos.max_bars) {
                    double exit_px = (pos.side == 1) ? (bar_c - cfg.inst.exit_slip_pts) : (bar_c + cfg.inst.exit_slip_pts);
                    realize_trade(pos, exit_px, b, 2);
                    pool[p_idx] = pool.back();
                    pool.pop_back();
                    continue;
                }

                // Update MFE strictly AFTER surviving bar b intrabar SL checks
                double fav = (pos.side == 1) ? (bar_h - pos.entry_px) : (pos.entry_px - bar_l);
                if (fav > pos.peak_mfe) pos.peak_mfe = fav;

                double new_stop = pos.stop_px;
                bool stop_moved = false;

                // Interbar Break-Even Lock
                if (pos.be_trig > 0.0 && pos.peak_mfe >= pos.be_trig) {
                    double be_px = (pos.side == 1) ? (pos.entry_px + pos.be_offset) : (pos.entry_px - pos.be_offset);
                    double q_be = quantize_stop_tick(be_px, pos.side, cfg.inst.tick_size);
                    if ((pos.side == 1 && q_be > new_stop) || (pos.side == -1 && q_be < new_stop)) {
                        new_stop = q_be;
                        stop_moved = true;
                    }
                }

                // Interbar Trailing Ratchet
                if (pos.r_trig > 0.0 && pos.peak_mfe >= pos.r_trig) {
                    double lock_dist = std::max(pos.r_trig - pos.r_trail, pos.peak_mfe - pos.r_trail);
                    double floor_px = (pos.side == 1) ? (pos.entry_px + lock_dist) : (pos.entry_px - lock_dist);
                    double q_r = quantize_stop_tick(floor_px, pos.side, cfg.inst.tick_size);
                    if ((pos.side == 1 && q_r > new_stop) || (pos.side == -1 && q_r < new_stop)) {
                        new_stop = q_r;
                        stop_moved = true;
                    }
                }

                if (stop_moved) {
                    if ((pos.side == 1 && bar_c <= new_stop) || (pos.side == -1 && bar_c >= new_stop)) {
                        double exit_px = (pos.side == 1) ? (bar_c - cfg.inst.exit_slip_pts) : (bar_c + cfg.inst.exit_slip_pts);
                        realize_trade(pos, exit_px, b, 0);
                        pool[p_idx] = pool.back();
                        pool.pop_back();
                        continue;
                    }
                    pos.stop_px = (pos.side == 1)
                        ? std::min(new_stop, bar_c - cfg.inst.tick_size)
                        : std::max(new_stop, bar_c + cfg.inst.tick_size);
                }
                ++p_idx;
            }

            if (collect_trades_and_days && is_active_day(cur_day)) {
                out.bar_samples.push_back({b, cur_day, day_pnl - day_pnl_before_bar,
                    std::min(day_pnl, worst_day_eq) - day_pnl_before_bar,
                    std::max(day_pnl, best_day_eq) - day_pnl_before_bar});
            }
            check_intraday_pass_on_trials(day_pnl);
            if (day_halted) continue;
        }

        // 4. Plugin Signal Evaluation at Completed Bar b Close (Queues for Bar b+1)
        if (!sess_halted[s_id] && (b + 1 < b_end) && !md.meta[b + 1].is_new_sess && !md.meta[b + 1].is_new_day) {
            CausalBarView view{
                &md, b, !pool.empty(), sess_entries_cnt[s_id],
                cfg.ref_nq, cfg.ref_es, cfg.ref_ym, ref_idx_nq, ref_idx_es, ref_idx_ym
            };
            PluginSignal sig = plugin.on_bar(view, plugin_state);
            if (sig.direction != 0 && sig.sl_pts > 0.0) {
                bool can_enter = true;
                if (cfg.max_concurrent_positions > 0 && static_cast<int>(pool.size()) >= cfg.max_concurrent_positions) {
                    can_enter = false;
                }
                if (can_enter && last_exit_bar[s_id] >= 0 &&
                    static_cast<int32_t>(b - static_cast<size_t>(last_exit_bar[s_id])) < sig.cooldown_bars) {
                    can_enter = false;
                }
                if (can_enter && sig.tp_pts > 0.0) {
                    double ref = (sig.order_type == 1 || sig.order_type == 2) ? sig.trigger_px : static_cast<double>(md.bars[b + 1].o);
                    double est_fill = (sig.direction == 1) ? (ref + cfg.inst.entry_slip_pts) : (ref - cfg.inst.entry_slip_pts);
                    double est_sl = (sig.direction == 1) ? (est_fill - sig.sl_pts) : (est_fill + sig.sl_pts);
                    double est_tp = (sig.direction == 1) ? (est_fill + sig.tp_pts) : (est_fill - sig.tp_pts);
                    if (!validate_structural_side(sig.direction, est_sl, est_tp)) {
                        can_enter = false;
                    }
                }
                if (can_enter) {
                    pending.push_back({s_id, sig});
                }
            }
        }
    }

    // Flush EOF positions and finalize last day
    if (b_end > b_begin) {
        if (!pool.empty()) {
            double before_flat = day_pnl;
            flatten_all_at_price(static_cast<double>(md.bars[b_end - 1].c), b_end - 1, 3);
            if (collect_trades_and_days && is_active_day(cur_day)) {
                double delta = day_pnl - before_flat;
                out.bar_samples.push_back({b_end - 1, cur_day, delta, std::min(0.0, delta), std::max(0.0, delta)});
            }
        }
        if (cur_day >= 0) {
            bool consec = hit_dll_today && prev_day_hit_dll;
            if (collect_trades_and_days && is_active_day(cur_day)) {
                DayRecord dr{};
                dr.cme_day_idx = cur_day;
                dr.cme_yyyymmdd = md.day_yyyymmdd[static_cast<size_t>(cur_day)];
                dr.day_pnl = day_pnl;
                dr.worst_intra_delta = day_worst_intra_delta;
                dr.hit_dll = hit_dll_today;
                dr.consec_dll = consec;
                dr.trades_cnt = day_trades_cnt;
                for (int s = 0; s < 4; ++s) dr.sess_pnl[s] = sess_pnl[s];
                dr.dll_sess_id = dll_sess_id_today;
                out.days.push_back(dr);
            }
            finalize_day_on_trials(day_pnl, hit_dll_today);
        }
    }

    if (plugin.destroy_state && plugin_state) {
        plugin.destroy_state(plugin_state);
    }
    return out;
}

inline ChunkOutput execute_chunked_core(
    const PrecomputedMarketData& md,
    const StrategyPlugin& plugin,
    const EngineConfig& cfg
) {
    ChunkOutput merged;
    if (md.total_days <= 0) return merged;

    const int n_chunks = std::max(1, std::min(cfg.num_chunks, md.total_days));
    std::vector<ChunkOutput> chunk_outs(static_cast<size_t>(n_chunks));

    if (n_chunks == 1) {
        chunk_outs[0] = simulate_chunk(md, plugin, cfg, 0, md.total_days - 1, true);
    } else {
        #pragma omp parallel for schedule(dynamic, 1)
        for (int c = 0; c < n_chunks; ++c) {
            int32_t d_start = static_cast<int32_t>((static_cast<int64_t>(c) * md.total_days) / n_chunks);
            int32_t d_end = static_cast<int32_t>((static_cast<int64_t>(c + 1) * md.total_days) / n_chunks) - 1;
            chunk_outs[static_cast<size_t>(c)] = simulate_chunk(md, plugin, cfg, d_start, d_end, true);
        }
    }

    for (auto& co : chunk_outs) {
        merged.trades.insert(merged.trades.end(), co.trades.begin(), co.trades.end());
        merged.bar_samples.insert(merged.bar_samples.end(), co.bar_samples.begin(), co.bar_samples.end());
        merged.days.insert(merged.days.end(), co.days.begin(), co.days.end());
        merged.combines.insert(merged.combines.end(), co.combines.begin(), co.combines.end());
    }
    return merged;
}

inline double normal_cdf(double x) {
    return 0.5 * std::erfc(-x * 0.70710678118654752440);
}

template <typename TradePred, typename DayPred>
inline SplitMetrics compute_split_metrics(
    const std::vector<TradeRecord>& trades,
    const std::vector<DayRecord>& days,
    TradePred t_pred,
    DayPred d_pred
) {
    SplitMetrics sm{};
    double gp = 0.0, gl = 0.0, pk = 0.0;
    for (const auto& tr : trades) {
        if (!t_pred(tr)) continue;
        sm.trades++;
        sm.net_pnl += tr.net_usd;
        if (sm.net_pnl > pk) pk = sm.net_pnl;
        sm.curve_max_dd = std::max(sm.curve_max_dd, pk - sm.net_pnl);
        if (tr.net_usd > 0.0) { sm.wins++; gp += tr.net_usd; }
        else { gl += std::abs(tr.net_usd); }
    }
    if (sm.trades > 0) {
        sm.win_rate_pct = 100.0 * static_cast<double>(sm.wins) / static_cast<double>(sm.trades);
        sm.profit_factor = (gl > 0.0) ? (gp / gl) : (gp > 0.0 ? 99.0 : 0.0);
    }
    double sum = 0.0, s2 = 0.0;
    size_t cnt = 0;
    for (const auto& d : days) {
        if (!d_pred(d)) continue;
        sum += d.day_pnl;
        cnt++;
    }
    if (cnt >= 2) {
        double mean = sum / static_cast<double>(cnt);
        for (const auto& d : days) {
            if (!d_pred(d)) continue;
            double diff = d.day_pnl - mean;
            s2 += diff * diff;
        }
        double sd = std::sqrt(s2 / static_cast<double>(cnt - 1));
        sm.sharpe_ann = (sd > 1e-12) ? ((mean / sd) * std::sqrt(252.0)) : 0.0;
    }
    return sm;
}

inline SplitMetrics compute_side_split_metrics(
    const std::vector<TradeRecord>& trades,
    const std::vector<DayRecord>& days,
    int8_t target_side
) {
    SplitMetrics sm{};
    double gp = 0.0, gl = 0.0, pk = 0.0;
    std::vector<double> daily_side_pnl(days.size(), 0.0);
    for (const auto& tr : trades) {
        if (tr.side != target_side) continue;
        sm.trades++;
        sm.net_pnl += tr.net_usd;
        if (sm.net_pnl > pk) pk = sm.net_pnl;
        sm.curve_max_dd = std::max(sm.curve_max_dd, pk - sm.net_pnl);
        if (tr.net_usd > 0.0) { sm.wins++; gp += tr.net_usd; }
        else { gl += std::abs(tr.net_usd); }
        if (tr.cme_day_idx >= 0 && static_cast<size_t>(tr.cme_day_idx) < daily_side_pnl.size()) {
            daily_side_pnl[static_cast<size_t>(tr.cme_day_idx)] += tr.net_usd;
        }
    }
    if (sm.trades > 0) {
        sm.win_rate_pct = 100.0 * static_cast<double>(sm.wins) / static_cast<double>(sm.trades);
        sm.profit_factor = (gl > 0.0) ? (gp / gl) : (gp > 0.0 ? 99.0 : 0.0);
    }
    if (daily_side_pnl.size() >= 2) {
        double sum = 0.0, s2 = 0.0;
        for (double x : daily_side_pnl) sum += x;
        double mean = sum / static_cast<double>(daily_side_pnl.size());
        for (double x : daily_side_pnl) {
            double diff = x - mean;
            s2 += diff * diff;
        }
        double sd = std::sqrt(s2 / static_cast<double>(daily_side_pnl.size() - 1));
        sm.sharpe_ann = (sd > 1e-12) ? ((mean / sd) * std::sqrt(252.0)) : 0.0;
    }
    return sm;
}

inline TradeMicrostructureMetrics compute_microstructure_metrics(
    const std::vector<TradeRecord>& trades,
    const std::vector<DayRecord>& days
) {
    TradeMicrostructureMetrics tm{};
    const size_t nt = trades.size();
    if (nt > 0) {
        long long win_cnt = 0, loss_cnt = 0;
        double sum_win_pts = 0.0, sum_loss_pts = 0.0;
        double sum_win_usd = 0.0, sum_loss_usd = 0.0;
        double bars_win = 0.0, bars_loss = 0.0, bars_all = 0.0;
        double sum_giveback = 0.0;
        int cur_cw = 0, cur_cl = 0;
        bool has_win = false, has_loss = false;

        for (const auto& tr : trades) {
            if (tr.pts_net > 0.0) tm.gross_profit_pts += tr.pts_net;
            else tm.gross_loss_pts += std::abs(tr.pts_net);

            double bh = (tr.exit_bar >= tr.entry_bar)
                ? static_cast<double>(tr.exit_bar - tr.entry_bar + 1)
                : 1.0;
            bars_all += bh;
            sum_giveback += (tr.peak_mfe_pts - tr.pts_net);

            if (tr.exit_reason < 7) {
                tm.exit_reason_counts[tr.exit_reason]++;
            }

            if (tr.net_usd > 0.0) {
                win_cnt++;
                tm.gross_profit_usd += tr.net_usd;
                sum_win_pts += tr.pts_net;
                sum_win_usd += tr.net_usd;
                bars_win += bh;
                if (!has_win || tr.pts_net > tm.max_win_pts) tm.max_win_pts = tr.pts_net;
                if (!has_win || tr.net_usd > tm.max_win_usd) tm.max_win_usd = tr.net_usd;
                has_win = true;
                cur_cw++;
                cur_cl = 0;
                if (cur_cw > tm.max_consec_wins) tm.max_consec_wins = cur_cw;
            } else {
                loss_cnt++;
                tm.gross_loss_usd += std::abs(tr.net_usd);
                sum_loss_pts += tr.pts_net;
                sum_loss_usd += tr.net_usd;
                bars_loss += bh;
                if (!has_loss || tr.pts_net < tm.max_loss_pts) tm.max_loss_pts = tr.pts_net;
                if (!has_loss || tr.net_usd < tm.max_loss_usd) tm.max_loss_usd = tr.net_usd;
                has_loss = true;
                cur_cl++;
                cur_cw = 0;
                if (cur_cl > tm.max_consec_losses) tm.max_consec_losses = cur_cl;
            }
        }

        const double n_d = static_cast<double>(nt);
        tm.expectancy_pts = (sum_win_pts + sum_loss_pts) / n_d;
        tm.expectancy_usd = (sum_win_usd + sum_loss_usd) / n_d;
        tm.avg_bars_held_all = bars_all / n_d;
        tm.avg_mfe_giveback_pts = sum_giveback / n_d;
        if (win_cnt > 0) {
            tm.avg_win_pts = sum_win_pts / static_cast<double>(win_cnt);
            tm.avg_win_usd = sum_win_usd / static_cast<double>(win_cnt);
            tm.avg_bars_held_wins = bars_win / static_cast<double>(win_cnt);
        }
        if (loss_cnt > 0) {
            tm.avg_loss_pts = sum_loss_pts / static_cast<double>(loss_cnt);
            tm.avg_loss_usd = sum_loss_usd / static_cast<double>(loss_cnt);
            tm.avg_bars_held_losses = bars_loss / static_cast<double>(loss_cnt);
        }
        tm.realized_payoff_ratio = (win_cnt > 0)
            ? (tm.avg_win_pts / std::max(1e-9, std::abs(tm.avg_loss_pts)))
            : 0.0;
        for (int r = 0; r < 7; ++r) {
            tm.exit_reason_pct[r] = 100.0 * static_cast<double>(tm.exit_reason_counts[r]) / n_d;
        }
    }

    int cur_cwd = 0, cur_cld = 0;
    for (const auto& d : days) {
        if (d.day_pnl > 0.0) {
            cur_cwd++;
            cur_cld = 0;
            if (cur_cwd > tm.max_consec_win_days) tm.max_consec_win_days = cur_cwd;
        } else if (d.day_pnl < 0.0) {
            cur_cld++;
            cur_cwd = 0;
            if (cur_cld > tm.max_consec_loss_days) tm.max_consec_loss_days = cur_cld;
        } else {
            cur_cwd = 0;
            cur_cld = 0;
        }
    }
    return tm;
}

inline StatisticalMetrics compute_statistical_metrics(
    const std::vector<TradeRecord>& trades,
    const std::vector<DayRecord>& days,
    double curve_max_dd,
    int dsr_num_trials
) {
    StatisticalMetrics sm{};
    const size_t nd = days.size();
    if (nd >= 2) {
        double sum = 0.0;
        for (const auto& d : days) sum += d.day_pnl;
        double mean = sum / static_cast<double>(nd);
        double var = 0.0, down_var = 0.0;
        double m3 = 0.0, m4 = 0.0;
        for (const auto& d : days) {
            double diff = d.day_pnl - mean;
            double d2 = diff * diff;
            var += d2;
            m3 += d2 * diff;
            m4 += d2 * d2;
            if (d.day_pnl < 0.0) down_var += d.day_pnl * d.day_pnl;
        }
        double sd = std::sqrt(var / static_cast<double>(nd - 1));
        double dsd = std::sqrt(down_var / static_cast<double>(nd));
        double sr_daily = (sd > 1e-12) ? (mean / sd) : 0.0;
        sm.sharpe_ann = sr_daily * std::sqrt(252.0);
        sm.clean_sharpe = sm.sharpe_ann;
        sm.sortino_ann = (dsd > 1e-12) ? ((mean / dsd) * std::sqrt(252.0)) : 0.0;
        double ann_ret = mean * 252.0;
        sm.calmar_ratio = (curve_max_dd > 1e-9) ? (ann_ret / curve_max_dd) : 0.0;

        // Skew & Excess Kurtosis for PSR/DSR & Tail Moments
        double var_pop = var / static_cast<double>(nd);
        double sd_pop = std::sqrt(var_pop);
        double skew = (sd_pop > 1e-12) ? ((m3 / static_cast<double>(nd)) / (sd_pop * sd_pop * sd_pop)) : 0.0;
        double kurt = (sd_pop > 1e-12) ? ((m4 / static_cast<double>(nd)) / (var_pop * var_pop)) : 3.0;
        sm.return_skewness = skew;
        sm.return_kurtosis = kurt - 3.0;
        double denom_sq = std::max(1e-9, 1.0 - skew * sr_daily + ((kurt - 1.0) / 4.0) * sr_daily * sr_daily);
        double z_psr = (sr_daily * std::sqrt(static_cast<double>(nd - 1))) / std::sqrt(denom_sq);
        sm.psr = normal_cdf(z_psr);

        // Deflated Sharpe Ratio
        int M = std::max(1, dsr_num_trials);
        double sr_bench = 0.0;
        if (M > 1) {
            constexpr double euler_gamma = 0.57721566490153286;
            double p1 = 1.0 - 1.0 / static_cast<double>(M);
            double p2 = 1.0 - 1.0 / (static_cast<double>(M) * 2.718281828459045);
            auto approx_inv_norm = [](double p) -> double {
                double t = std::sqrt(-2.0 * std::log(std::max(1e-12, 1.0 - p)));
                return t - (2.515517 + 0.802853 * t + 0.010328 * t * t) /
                           (1.0 + 1.432788 * t + 0.189269 * t * t + 0.001308 * t * t * t);
            };
            double z1 = approx_inv_norm(p1);
            double z2 = approx_inv_norm(p2);
            double e_max_z = (1.0 - euler_gamma) * z1 + euler_gamma * z2;
            sr_bench = e_max_z / std::sqrt(static_cast<double>(nd));
        }
        double z_dsr = ((sr_daily - sr_bench) * std::sqrt(static_cast<double>(nd - 1))) / std::sqrt(denom_sq);
        sm.dsr = normal_cdf(z_dsr);

        // Ulcer Index + Underwater Geometry on Daily Equity Curve
        double eq = 0.0, pk = 0.0, sum_sq_dd = 0.0, sum_sq_pct = 0.0;
        int uw_days = 0, cur_uw = 0, max_uw = 0;
        std::vector<double> eq_curve(nd, 0.0);
        for (size_t i = 0; i < nd; ++i) {
            eq += days[i].day_pnl;
            eq_curve[i] = eq;
            if (eq > pk) pk = eq;
            double dd = pk - eq;
            if (dd > 1e-9) {
                uw_days++;
                cur_uw++;
                if (cur_uw > max_uw) max_uw = cur_uw;
            } else {
                cur_uw = 0;
            }
            sum_sq_dd += dd * dd;
            double dd_pct = 100.0 * dd / 50000.0;
            sum_sq_pct += dd_pct * dd_pct;
        }
        sm.ulcer_index_usd = std::sqrt(sum_sq_dd / static_cast<double>(nd));
        sm.ulcer_index_pct = std::sqrt(sum_sq_pct / static_cast<double>(nd));
        sm.underwater_pct = 100.0 * static_cast<double>(uw_days) / static_cast<double>(nd);
        sm.max_underwater_days = max_uw;
        if (curve_max_dd <= 1e-9) sm.max_dd_pct_of_pnl = 0.0;
        else if (sum > 1e-9) sm.max_dd_pct_of_pnl = 100.0 * curve_max_dd / sum;
        else sm.max_dd_pct_of_pnl = 999.0;

        // Equity Curve Linear & Centered Quadratic R^2 + Acceleration
        {
            const double n_d = static_cast<double>(nd);
            double x_bar = 0.5 * (n_d - 1.0);
            double y_bar = 0.0;
            for (double y : eq_curve) y_bar += y;
            y_bar /= n_d;

            double u2_bar = 0.0;
            for (size_t i = 0; i < nd; ++i) {
                double u = static_cast<double>(i) - x_bar;
                u2_bar += u * u;
            }
            u2_bar /= n_d;

            double s_uu = 0.0, s_uv = 0.0, s_vv = 0.0;
            double s_uy = 0.0, s_vy = 0.0, ss_tot = 0.0;
            for (size_t i = 0; i < nd; ++i) {
                double u = static_cast<double>(i) - x_bar;
                double v = (u * u) - u2_bar;
                double dy = eq_curve[i] - y_bar;
                s_uu += u * u;
                s_uv += u * v;
                s_vv += v * v;
                s_uy += u * dy;
                s_vy += v * dy;
                ss_tot += dy * dy;
            }
            if (ss_tot > 1e-12 && s_uu > 1e-12) {
                double ss_reg_lin = (s_uy * s_uy) / s_uu;
                sm.r2_linear = std::clamp(ss_reg_lin / ss_tot, 0.0, 1.0);
                double det = s_uu * s_vv - s_uv * s_uv;
                if (det > 1e-12) {
                    double b1 = (s_vv * s_uy - s_uv * s_vy) / det;
                    double b2 = (s_uu * s_vy - s_uv * s_uy) / det;
                    double ss_reg_q = b1 * s_uy + b2 * s_vy;
                    sm.r2_quadratic = std::clamp(ss_reg_q / ss_tot, sm.r2_linear, 1.0);
                    sm.quadratic_acceleration = b2;
                } else {
                    sm.r2_quadratic = sm.r2_linear;
                    sm.quadratic_acceleration = 0.0;
                }
            }
        }

        // Lo-MacKinlay VR(2) + Z-stat + p-value, and VR(5) + Hurst Exponent
        if (nd >= 10 && var_pop > 1e-12) {
            // VR(2) with heteroskedasticity-consistent Z*(2)
            double sum2 = 0.0, sum2_sq = 0.0;
            size_t n2 = nd - 1;
            for (size_t i = 0; i < n2; ++i) {
                double s2_val = days[i].day_pnl + days[i + 1].day_pnl;
                sum2 += s2_val;
                sum2_sq += s2_val * s2_val;
            }
            double mean2 = sum2 / static_cast<double>(n2);
            double var2 = std::max(0.0, (sum2_sq / static_cast<double>(n2)) - mean2 * mean2);
            sm.lo_mackinlay_vr2 = var2 / (2.0 * var_pop);

            double delta1_num = 0.0;
            for (size_t i = 1; i < nd; ++i) {
                double d_cur = days[i].day_pnl - mean;
                double d_prv = days[i - 1].day_pnl - mean;
                delta1_num += (d_cur * d_cur) * (d_prv * d_prv);
            }
            double delta1 = (var > 1e-12) ? (delta1_num / (var * var)) : 0.0;
            if (delta1 > 1e-15) {
                sm.vr_z_stat = (sm.lo_mackinlay_vr2 - 1.0) / std::sqrt(delta1);
                sm.vr_p_value = std::erfc(std::abs(sm.vr_z_stat) * 0.70710678118654752440);
            } else {
                sm.vr_z_stat = 0.0;
                sm.vr_p_value = 1.0;
            }
        }

        if (nd >= 20 && var_pop > 1e-12) {
            const int q = 5;
            double sum_q = 0.0, sum_q2 = 0.0;
            size_t n_q = nd - q + 1;
            for (size_t i = 0; i < n_q; ++i) {
                double s_q = 0.0;
                for (int j = 0; j < q; ++j) s_q += days[i + static_cast<size_t>(j)].day_pnl;
                sum_q += s_q;
                sum_q2 += s_q * s_q;
            }
            double mean_q = sum_q / static_cast<double>(n_q);
            double var_q = std::max(1e-12, (sum_q2 / static_cast<double>(n_q)) - mean_q * mean_q);
            sm.variance_ratio_5d = var_q / (static_cast<double>(q) * var_pop);
            sm.hurst_exponent = std::max(0.01, std::min(0.99, 0.5 + 0.5 * std::log(std::max(1e-6, sm.variance_ratio_5d)) / std::log(static_cast<double>(q))));
        }

        // Markov P(DLL_{t+1} | DLL_t)
        long long dll_prev = 0, dll_dll = 0;
        for (size_t i = 0; i + 1 < nd; ++i) {
            if (days[i].hit_dll) {
                dll_prev++;
                if (days[i + 1].hit_dll) dll_dll++;
            }
        }
        sm.markov_p_dll_given_dll = (dll_prev > 0)
            ? (static_cast<double>(dll_dll) / static_cast<double>(dll_prev))
            : 0.0;
    }

    // Markov [P(W|W), P(L|L), ClusteringBias] & Bayesian Win-Rate 95% CI
    if (!trades.empty()) {
        long long w_cnt = 0, l_cnt = 0;
        long long ww = 0, w_prev = 0, ll = 0, l_prev = 0;
        for (size_t i = 0; i < trades.size(); ++i) {
            bool win = trades[i].net_usd > 0.0;
            if (win) w_cnt++; else l_cnt++;
            if (i > 0) {
                bool prev_win = trades[i - 1].net_usd > 0.0;
                if (prev_win) { w_prev++; if (win) ww++; }
                else { l_prev++; if (!win) ll++; }
            }
        }
        sm.markov_p_ww = (w_prev > 0) ? (static_cast<double>(ww) / static_cast<double>(w_prev)) : 0.0;
        sm.markov_p_ll = (l_prev > 0) ? (static_cast<double>(ll) / static_cast<double>(l_prev)) : 0.0;
        sm.markov_clustering_bias = sm.markov_p_ww - (1.0 - sm.markov_p_ll);
        double alpha = 1.0 + static_cast<double>(w_cnt);
        double beta  = 1.0 + static_cast<double>(l_cnt);
        double n_ab  = alpha + beta;
        double mu = alpha / n_ab;
        double sd_b = std::sqrt((alpha * beta) / (n_ab * n_ab * (n_ab + 1.0)));
        sm.bayes_wr_mean = 100.0 * mu;
        sm.bayes_wr_ci_low  = 100.0 * std::max(0.0, mu - 1.96 * sd_b);
        sm.bayes_wr_ci_high = 100.0 * std::min(1.0, mu + 1.96 * sd_b);
    }
    return sm;
}

inline FundedPolicyMetrics simulate_funded_policy(
    const std::vector<DayRecord>& days,
    bool is_policy_b
) {
    FundedPolicyMetrics fm{};
    if (days.size() < 20) return fm;

    const double min_win_day = is_policy_b ? 200.0 : 150.0;
    const double req_balance_before_payout = is_policy_b ? 2500.0 : 1000.0;
    const double leave_buffer = is_policy_b ? 2000.0 : 0.0;

    // Non-overlapping / rolling monthly-spaced cycles every 21 trading days
    for (size_t s = 0; s + 20 < days.size(); s += 21) {
        fm.cycles_tested++;
        double bal = 0.0;
        double eod_peak = 0.0;
        int win_days = 0;
        bool prev_dll = false;
        bool blown = false;
        double cycle_payouts = 0.0;

        size_t horizon = std::min(days.size(), s + 252);
        for (size_t i = s; i < horizon; ++i) {
            const auto& d = days[i];
            if (d.hit_dll && prev_dll) { blown = true; break; }
            prev_dll = d.hit_dll;

            // Express Funded MLL: trails EOD peak up to $2,000, then locks at $0
            double mll_floor = std::min(0.0, eod_peak - 2000.0);
            if (bal + d.worst_intra_delta <= mll_floor) { blown = true; break; }

            bal += d.day_pnl;
            if (bal <= mll_floor) { blown = true; break; }
            if (bal > eod_peak) eod_peak = bal;
            if (d.day_pnl >= min_win_day) win_days++;

            if (win_days >= 5 && bal >= req_balance_before_payout) {
                double max_w = std::min(5000.0, (bal - leave_buffer) * 0.50);
                if (max_w >= 250.0) {
                    bal -= max_w;
                    cycle_payouts += max_w;
                    fm.payout_count++;
                    win_days = 0;
                }
            }
        }
        if (blown) fm.blown_count++;
        fm.total_payouts_usd += cycle_payouts;
    }

    if (fm.cycles_tested > 0) {
        fm.survival_rate_pct = 100.0 * static_cast<double>(fm.cycles_tested - fm.blown_count) / static_cast<double>(fm.cycles_tested);
        if (fm.payout_count > 0) {
            fm.avg_payout_usd = fm.total_payouts_usd / static_cast<double>(fm.payout_count);
        }
        // Avg total payout per cycle divided by ~12 months
        fm.expected_monthly_payout_usd = (fm.total_payouts_usd / static_cast<double>(fm.cycles_tested)) / 12.0;
    }
    return fm;
}

inline BootstrapMetrics run_block_bootstrap_mc(
    const std::vector<DayRecord>& days,
    const EngineConfig& cfg
) {
    BootstrapMetrics bm{};
    bm.iterations = cfg.mc_iterations;
    if (days.empty() || cfg.mc_iterations <= 0) return bm;

    const int n_days = static_cast<int>(days.size());
    const int blk = std::max(1, std::min(cfg.mc_block_days, n_days));
    const int horizon = std::max(5, cfg.max_trial_days);

    std::vector<double> final_eq(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> max_dds(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> mc_sharpes(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> mc_pnls(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> mc_wrs(static_cast<size_t>(cfg.mc_iterations), 0.0);
    long long passes = 0, ruins = 0, prof = 0, pass_2d = 0, pass_5d = 0, sum_ttp = 0;

    #pragma omp parallel for reduction(+:passes,ruins,prof,pass_2d,pass_5d,sum_ttp) schedule(static)
    for (int it = 0; it < cfg.mc_iterations; ++it) {
        uint64_t rng = cfg.mc_seed ^ (static_cast<uint64_t>(it + 1) * 0x9E3779B97F4A7C15ULL);
        auto next_u32 = [&]() -> uint32_t {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            return static_cast<uint32_t>((rng * 0x2545F4914F6CDD1DULL) >> 32);
        };

        double eq = 0.0, pk = 0.0, mdd = 0.0, best_day = 0.0;
        bool passed = false, ruined = false, prev_dll = false;
        int pass_day = 0;

        int d = 0;
        while (d < horizon && !passed && !ruined) {
            int start_idx = static_cast<int>(next_u32() % static_cast<uint32_t>(n_days));
            for (int k = 0; k < blk && d < horizon; ++k, ++d) {
                const auto& rec = days[static_cast<size_t>((start_idx + k) % n_days)];
                if (rec.hit_dll && prev_dll) { ruined = true; break; }
                prev_dll = rec.hit_dll;

                double intra_eq = eq + rec.worst_intra_delta;
                double intra_pk = std::max(pk, intra_eq);
                double intra_dd = intra_pk - intra_eq;
                if (intra_dd > mdd) mdd = intra_dd;
                if ((cfg.enforce_intrabar_mll && intra_dd >= cfg.mll_limit_usd) || intra_eq <= -cfg.mll_limit_usd) {
                    ruined = true;
                    break;
                }

                eq += rec.day_pnl;
                if (rec.day_pnl > best_day) best_day = rec.day_pnl;
                if (eq > pk) pk = eq;
                double dd = pk - eq;
                if (dd > mdd) mdd = dd;
                if (mdd >= cfg.mll_limit_usd || eq <= -cfg.mll_limit_usd) {
                    ruined = true;
                    break;
                }
                if (eq >= cfg.target_profit_usd && (d + 1) >= 2 && best_day <= cfg.consistency_max_pct * eq) {
                    passed = true;
                    pass_day = d + 1;
                    break;
                }
            }
        }
        if (passed) {
            passes++;
            sum_ttp += static_cast<long long>(pass_day);
            if (pass_day <= 2) pass_2d++;
            if (pass_day <= 5) pass_5d++;
        }
        if (ruined) ruins++;
        if (eq > 0.0) prof++;
        final_eq[static_cast<size_t>(it)] = eq;
        max_dds[static_cast<size_t>(it)] = mdd;

        // Full-series block bootstrap for 95% CI on Sharpe, Net PnL, Daily Win Rate
        double sum_p = 0.0, sum_p2 = 0.0;
        int win_d = 0, act_d = 0;
        int fd = 0;
        while (fd < n_days) {
            int start_idx = static_cast<int>(next_u32() % static_cast<uint32_t>(n_days));
            for (int k = 0; k < blk && fd < n_days; ++k, ++fd) {
                const auto& rec = days[static_cast<size_t>((start_idx + k) % n_days)];
                sum_p += rec.day_pnl;
                sum_p2 += rec.day_pnl * rec.day_pnl;
                if (rec.trades_cnt > 0) {
                    act_d++;
                    if (rec.day_pnl > 0.0) win_d++;
                }
            }
        }
        mc_pnls[static_cast<size_t>(it)] = sum_p;
        if (n_days >= 2) {
            double mean_p = sum_p / static_cast<double>(n_days);
            double var_p = std::max(0.0, (sum_p2 - sum_p * mean_p) / static_cast<double>(n_days - 1));
            double sd_p = std::sqrt(var_p);
            mc_sharpes[static_cast<size_t>(it)] = (sd_p > 1e-12) ? ((mean_p / sd_p) * std::sqrt(252.0)) : 0.0;
        }
        mc_wrs[static_cast<size_t>(it)] = (act_d > 0)
            ? (100.0 * static_cast<double>(win_d) / static_cast<double>(act_d))
            : 0.0;
    }

    const double nit = static_cast<double>(cfg.mc_iterations);
    bm.pass_rate_pct = 100.0 * static_cast<double>(passes) / nit;
    bm.ruin_rate_pct = 100.0 * static_cast<double>(ruins) / nit;
    bm.profitable_pct = 100.0 * static_cast<double>(prof) / nit;
    bm.pass_2d_pct = 100.0 * static_cast<double>(pass_2d) / nit;
    bm.pass_5d_pct = 100.0 * static_cast<double>(pass_5d) / nit;
    bm.mean_ttp = (passes > 0) ? (static_cast<double>(sum_ttp) / static_cast<double>(passes)) : 0.0;
    calc_ci95(std::move(mc_sharpes), bm.ci95_sharpe_low, bm.ci95_sharpe_high);
    calc_ci95(std::move(mc_pnls), bm.ci95_pnl_low, bm.ci95_pnl_high);
    calc_ci95(std::move(mc_wrs), bm.ci95_wr_low, bm.ci95_wr_high);
    bm.final_equity = calc_percentiles(std::move(final_eq));
    bm.max_dd = calc_percentiles(std::move(max_dds));
    return bm;
}

inline BootstrapMetrics run_noise_injection_bootstrap(
    const std::vector<TradeRecord>& trades,
    int32_t total_days,
    const EngineConfig& cfg,
    double* mean_noise_sharpe_out = nullptr
) {
    BootstrapMetrics bm{};
    bm.iterations = cfg.mc_iterations;
    if (mean_noise_sharpe_out) *mean_noise_sharpe_out = 0.0;
    if (trades.empty() || cfg.mc_iterations <= 0) return bm;

    const size_t nt = trades.size();
    const double pv = resolve_pv_per_micro_unit(cfg);
    const int ndays = std::max(1, total_days);
    std::vector<double> final_eq(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> max_dds(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> mc_sharpes(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> mc_pnls(static_cast<size_t>(cfg.mc_iterations), 0.0);
    std::vector<double> mc_wrs(static_cast<size_t>(cfg.mc_iterations), 0.0);
    long long passes = 0, ruins = 0, prof = 0, pass_2d = 0, pass_5d = 0, combine_pass_cnt = 0, sum_ttp = 0;

    #pragma omp parallel for reduction(+:passes,ruins,prof,pass_2d,pass_5d,combine_pass_cnt,sum_ttp) schedule(static)
    for (int it = 0; it < cfg.mc_iterations; ++it) {
        uint64_t rng = (cfg.mc_seed + 7919ULL) ^ (static_cast<uint64_t>(it + 1) * 0xBF58476D1CE4E5B9ULL);
        auto next_u01 = [&]() -> double {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            uint64_t x = rng * 0x2545F4914F6CDD1DULL;
            return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
        };

        double eq = 0.0, pk = 0.0, mdd = 0.0;
        long long win_tr = 0;
        int32_t cur_day = -1;
        double cur_day_pnl = 0.0, best_day_pnl = 0.0;
        double sum_d = 0.0, sum_d2 = 0.0;
        int first_day_idx = trades[0].cme_day_idx;
        int pass_day = 0;
        bool passed_combine = false;

        for (size_t i = 0; i < nt; ++i) {
            // Fast uniform zero-mean jitter with stdev = cfg.noise_jitter_pts (range [-sqrt(3)*s, +sqrt(3)*s])
            double jitter_pts = (next_u01() * 2.0 - 1.0) * 1.7320508 * cfg.noise_jitter_pts;
            double usd = trades[i].net_usd + jitter_pts * pv * trades[i].contracts_micros;
            if (usd > 0.0) win_tr++;
            eq += usd;
            if (eq > pk) pk = eq;
            double dd = pk - eq;
            if (dd > mdd) mdd = dd;

            int32_t d_idx = trades[i].cme_day_idx;
            if (d_idx != cur_day) {
                if (cur_day >= 0) {
                    sum_d += cur_day_pnl;
                    sum_d2 += cur_day_pnl * cur_day_pnl;
                    if (cur_day_pnl > best_day_pnl) best_day_pnl = cur_day_pnl;
                }
                cur_day = d_idx;
                cur_day_pnl = usd;
            } else {
                cur_day_pnl += usd;
            }
            if (!passed_combine && eq >= cfg.target_profit_usd) {
                int elapsed = (d_idx - first_day_idx) + 1;
                double hypo_best = std::max(best_day_pnl, cur_day_pnl);
                if (elapsed >= 2 && hypo_best <= cfg.consistency_max_pct * eq) {
                    passed_combine = true;
                    pass_day = elapsed;
                }
            }
        }
        if (cur_day >= 0) {
            sum_d += cur_day_pnl;
            sum_d2 += cur_day_pnl * cur_day_pnl;
        }

        double sr_it = 0.0;
        if (ndays >= 2) {
            double mean_d = sum_d / static_cast<double>(ndays);
            double var_d = std::max(0.0, (sum_d2 - sum_d * mean_d) / static_cast<double>(ndays - 1));
            double sd_d = std::sqrt(var_d);
            sr_it = (sd_d > 1e-12) ? ((mean_d / sd_d) * std::sqrt(252.0)) : 0.0;
        }

        if (eq > 0.0) prof++;
        if (eq >= cfg.target_profit_usd) passes++;
        if (passed_combine) {
            combine_pass_cnt++;
            sum_ttp += static_cast<long long>(pass_day);
            if (pass_day <= 2) pass_2d++;
            if (pass_day <= 5) pass_5d++;
        }
        if (mdd >= cfg.mll_limit_usd) ruins++;
        final_eq[static_cast<size_t>(it)] = eq;
        max_dds[static_cast<size_t>(it)] = mdd;
        mc_sharpes[static_cast<size_t>(it)] = sr_it;
        mc_pnls[static_cast<size_t>(it)] = eq;
        mc_wrs[static_cast<size_t>(it)] = 100.0 * static_cast<double>(win_tr) / static_cast<double>(nt);
    }

    const double nit = static_cast<double>(cfg.mc_iterations);
    bm.pass_rate_pct = 100.0 * static_cast<double>(passes) / nit;
    bm.ruin_rate_pct = 100.0 * static_cast<double>(ruins) / nit;
    bm.profitable_pct = 100.0 * static_cast<double>(prof) / nit;
    bm.pass_2d_pct = 100.0 * static_cast<double>(pass_2d) / nit;
    bm.pass_5d_pct = 100.0 * static_cast<double>(pass_5d) / nit;
    bm.mean_ttp = (combine_pass_cnt > 0) ? (static_cast<double>(sum_ttp) / static_cast<double>(combine_pass_cnt)) : 0.0;
    if (mean_noise_sharpe_out) {
        double sum_sharpe = 0.0;
        for (double sr : mc_sharpes) sum_sharpe += sr;
        *mean_noise_sharpe_out = sum_sharpe / nit;
    }
    calc_ci95(std::move(mc_sharpes), bm.ci95_sharpe_low, bm.ci95_sharpe_high);
    calc_ci95(std::move(mc_pnls), bm.ci95_pnl_low, bm.ci95_pnl_high);
    calc_ci95(std::move(mc_wrs), bm.ci95_wr_low, bm.ci95_wr_high);
    bm.final_equity = calc_percentiles(std::move(final_eq));
    bm.max_dd = calc_percentiles(std::move(max_dds));
    return bm;
}

} // namespace detail

// ----------------------------------------------------------------------------
// Size-Invariant 4-Tier & Account-Suitability Classifier
// ----------------------------------------------------------------------------
inline void classify_strategy(Universal11StageResult& res, const EngineConfig& cfg) {
    (void)cfg;
    auto& ic = res.inst;

    const bool has_min_trades = (res.total_trades >= 30);
    const bool has_raw_edge = has_min_trades && (res.profit_factor > 1.0) &&
                              (res.net_pts > 0.0) && (res.micro.expectancy_pts > 0.0);

    // 4-Tier Classification (Size-Invariant Gates)
    if (!has_raw_edge || (res.stats.psr < 0.70 && res.stats.sharpe_ann < 0.50)) {
        ic.tier = "TIER_4_DISQUALIFIED_NO_CAUSAL_EDGE";
    } else {
        const bool tier1_core =
            (res.profit_factor >= 1.15) &&
            (res.micro.expectancy_pts >= 0.50) &&
            (res.stats.sharpe_ann >= 1.0) &&
            (res.stats.psr >= 0.95) &&
            (res.h1.net_pnl > 0.0 && res.h2.net_pnl > 0.0) &&
            (res.h1.profit_factor > 1.0 && res.h2.profit_factor > 1.0) &&
            (ic.total_years_count > 0 && ic.positive_years_count * 10 >= ic.total_years_count * 7) &&
            (ic.pf_ex_top10 > 1.0 && ic.net_pts_ex_top10 > 0.0);

        const bool tier1_prop =
            (!cfg.run_full_11_stages) ||
            ((res.combine_m_safe.pass_rate_pct >= 50.0) &&
             (res.combine_m_safe.single_dll_days <= 100) &&
             (res.mc_block_30k_m_safe.pass_rate_pct >= 50.0 || res.mc_block_30k.pass_rate_pct >= 50.0) &&
             (res.stats.edge_retention_pct >= 80.0) &&
             (res.policy_a_m_safe.survival_rate_pct >= 50.0 || res.policy_b_m_safe.survival_rate_pct >= 50.0 ||
              res.policy_a.survival_rate_pct >= 50.0 || res.policy_b.survival_rate_pct >= 50.0));

        bool has_strong_session = false;
        for (int s = 0; s < 4; ++s) {
            if (res.sess_quant[s].trades >= 30 && res.sess_quant[s].profit_factor >= 1.15) {
                has_strong_session = true;
            }
        }

        const bool tier2_building_block =
            (res.profit_factor >= 1.05) &&
            (res.stats.psr >= 0.85) &&
            (res.h1.net_pnl > 0.0 || res.h2.net_pnl > 0.0) &&
            (ic.pf_ex_top10 > 1.0 || has_strong_session);

        if (tier1_core && tier1_prop) {
            ic.tier = "TIER_1_STANDALONE_CHAMPION";
        } else if (tier2_building_block) {
            ic.tier = "TIER_2_SESSION_SNIPER_BUILDING_BLOCK";
        } else {
            ic.tier = "TIER_3_NEEDS_STAGE5_EXIT_OR_SESSION_TUNING";
        }
    }

    // 14 Pipe-Delimited Account-Suitability & Behavioral Badges
    std::vector<std::string> b;
    const auto& cm_safe = (res.combine_m_safe.total_trials > 0) ? res.combine_m_safe : res.combine_configured;
    const auto& pa_safe = (res.policy_a_m_safe.cycles_tested > 0) ? res.policy_a_m_safe : res.policy_a;
    const auto& pb_safe = (res.policy_b_m_safe.cycles_tested > 0) ? res.policy_b_m_safe : res.policy_b;

    if (cm_safe.total_trials > 0 && cm_safe.pass_rate_pct >= 60.0 &&
        ((cm_safe.ttp.p50 > 0.0 && cm_safe.ttp.p50 <= 10.0) || cm_safe.day5_pass_pct >= 30.0)) {
        b.emplace_back("FAST_COMBINE_SPRINTER");
    }
    if (cm_safe.total_trials > 0 && cm_safe.pass_rate_pct >= 55.0 &&
        cm_safe.fail_mll_cnt <= static_cast<int>(0.25 * cm_safe.total_trials) &&
        cm_safe.single_dll_days <= 100) {
        b.emplace_back("SAFE_CONSISTENCY_EVAL");
    }
    if ((pa_safe.survival_rate_pct >= 60.0 && pa_safe.expected_monthly_payout_usd >= 300.0) ||
        (pb_safe.survival_rate_pct >= 60.0 && pb_safe.expected_monthly_payout_usd >= 300.0)) {
        b.emplace_back("FUNDED_XFA_PAYOUT_ENGINE");
    }
    if (res.total_trades > 0 &&
        res.micro.avg_mfe_giveback_pts <= std::max(8.0, 2.5 * std::max(0.0, res.micro.expectancy_pts)) &&
        (cm_safe.total_trials == 0 || cm_safe.intra_dd.p95 < 1500.0)) {
        b.emplace_back("INTRADAY_TRAILING_SAFE");
    }
    if (cm_safe.total_trials > 0 && cm_safe.eod_dd.p95 < 1500.0 &&
        cm_safe.fail_mll_cnt < static_cast<int>(0.40 * cm_safe.total_trials)) {
        b.emplace_back("EOD_TRAILING_SAFE");
    }
    if (has_raw_edge && (res.curve_max_dd_configured < 2000.0 ||
        (cm_safe.total_trials > 0 && cm_safe.pass_rate_pct >= 40.0 && cm_safe.eod_dd.p95 < 2000.0))) {
        b.emplace_back("STATIC_DD_SAFE");
    }
    if (cm_safe.total_trials > 0 && cm_safe.pass_rate_pct >= 50.0 && res.stats.r2_linear >= 0.70) {
        b.emplace_back("CONSISTENCY_NATIVE");
    }
    if (has_raw_edge && ic.breakeven_slip_pts >= 5.0 &&
        (!cfg.run_full_11_stages || res.stats.edge_retention_pct >= 80.0)) {
        b.emplace_back("MARKET_ORDER_ROBUST");
    }
    if (res.profit_factor > 1.0 && ic.breakeven_slip_pts < 5.0) {
        b.emplace_back("LIMIT_ORDER_REQUIRED");
    }
    if (ic.long_split.trades >= 15 && ic.short_split.trades >= 15 &&
        ic.long_split.profit_factor >= 1.05 && ic.short_split.profit_factor >= 1.05) {
        b.emplace_back("BIDIRECTIONAL_ALL_WEATHER");
    }
    if ((ic.long_split.net_pnl > 0.0 && (ic.short_split.trades < 10 || ic.short_split.profit_factor < 1.0)) ||
        (ic.long_split.net_pnl > 3.0 * std::max(1.0, ic.short_split.net_pnl))) {
        b.emplace_back("BULL_DRIFT_BIASED");
    }
    if (ic.bear_2022_split.trades >= 10 && ic.bear_2022_split.net_pnl > 0.0 &&
        ic.bear_2022_split.profit_factor >= 1.10) {
        b.emplace_back("BEAR_CRASH_HEDGE");
    }
    if (res.total_trades >= 10 && res.net_pts > 1e-9 &&
        (ic.top10_trades_pct_of_pnl >= 50.0 || ic.pf_ex_top10 < 1.0 || ic.net_pts_ex_top10 <= 0.0)) {
        b.emplace_back("OUTLIER_DEPENDENT_LOTTERY");
    }
    if (std::abs(ic.corr_vs_benchmark) <= 0.25 && res.total_trades >= 20 && res.profit_factor > 1.0) {
        b.emplace_back("ORTHOGONAL_DIVERSIFIER");
    }

    ic.badges.clear();
    for (size_t i = 0; i < b.size(); ++i) {
        if (i > 0) ic.badges += '|';
        ic.badges += b[i];
    }
}

inline void apply_sp500_benchmark(
    Universal11StageResult& res,
    const SP500Benchmark& sp,
    double account_base_usd = 50000.0
) {
    auto& ic = res.inst;
    ic.sp500_cagr_pct = sp.cagr_pct;
    ic.sp500_sharpe_ann = sp.sharpe_ann;
    ic.sp500_calmar_ratio = sp.calmar_ratio;
    ic.sp500_max_dd_pct = sp.max_dd_pct;

    if (res.days.size() < 2 || sp.yyyymmdd.size() < 2 || account_base_usd <= 0.0) return;

    std::vector<double> r_strat, r_sp;
    std::vector<double> r_sess[4];
    r_strat.reserve(std::min(res.days.size(), sp.yyyymmdd.size()));
    r_sp.reserve(r_strat.capacity());
    for (int s = 0; s < 4; ++s) r_sess[s].reserve(r_strat.capacity());

    int32_t first_dt = 0, last_dt = 0;
    size_t ia = 0, ib = 0;
    while (ia < res.days.size() && ib < sp.yyyymmdd.size()) {
        if (res.days[ia].cme_yyyymmdd == sp.yyyymmdd[ib]) {
            if (first_dt == 0) first_dt = res.days[ia].cme_yyyymmdd;
            last_dt = res.days[ia].cme_yyyymmdd;
            r_strat.push_back(res.days[ia].day_pnl / account_base_usd);
            r_sp.push_back(sp.daily_ret[ib]);
            for (int s = 0; s < 4; ++s) {
                r_sess[s].push_back(res.days[ia].sess_pnl[s] / account_base_usd);
            }
            ++ia; ++ib;
        } else if (res.days[ia].cme_yyyymmdd < sp.yyyymmdd[ib]) {
            ++ia;
        } else {
            ++ib;
        }
    }

    const size_t n = r_strat.size();
    if (n < 2) return;

    double years = exact_calendar_years(first_dt, last_dt);
    double days_per_yr = static_cast<double>(n) / years;

    double m_st = 0.0, m_sp = 0.0;
    for (size_t i = 0; i < n; ++i) { m_st += r_strat[i]; m_sp += r_sp[i]; }
    m_st /= static_cast<double>(n);
    m_sp /= static_cast<double>(n);

    double cov = 0.0, var_st = 0.0, var_sp = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double ds = r_strat[i] - m_st;
        double db = r_sp[i] - m_sp;
        cov += ds * db;
        var_st += ds * ds;
        var_sp += db * db;
    }

    double denom_corr = std::sqrt(var_st * var_sp);
    ic.corr_vs_benchmark = (denom_corr > 1e-12) ? (cov / denom_corr) : 0.0;
    ic.beta_vs_sp500 = (var_sp > 1e-15) ? (cov / var_sp) : 0.0;
    double alpha_daily = m_st - ic.beta_vs_sp500 * m_sp;
    ic.jensen_alpha_ann_pct = alpha_daily * days_per_yr * 100.0;

    double strat_years = exact_calendar_years(res.days.front().cme_yyyymmdd, res.days.back().cme_yyyymmdd);
    ic.ann_return_m_safe_pct = ((res.net_pnl_m_safe / account_base_usd) / strat_years) * 100.0;
    ic.beats_sp500_sharpe = (res.stats.sharpe_ann > sp.sharpe_ann);
    ic.beats_sp500_calmar = (res.stats.calmar_ratio > 2.0 * sp.calmar_ratio);
    ic.beats_sp500_cagr_m_safe = (ic.ann_return_m_safe_pct > sp.cagr_pct);

    bool any_sess_passes = false;
    for (int s = 0; s < 4; ++s) {
        if (res.sess_quant[s].trades < 30 || res.sess_quant[s].curve_max_dd <= 1e-9) continue;
        double m_ss = 0.0;
        for (double x : r_sess[s]) m_ss += x;
        m_ss /= static_cast<double>(n);
        double cov_ss = 0.0;
        for (size_t i = 0; i < n; ++i) {
            cov_ss += (r_sess[s][i] - m_ss) * (r_sp[i] - m_sp);
        }
        double beta_ss = (var_sp > 1e-15) ? (cov_ss / var_sp) : 0.0;
        double alpha_ss = (m_ss - beta_ss * m_sp) * days_per_yr * 100.0;
        double sess_calmar = (m_ss * account_base_usd * 252.0) / res.sess_quant[s].curve_max_dd;
        if (sess_calmar > sp.calmar_ratio && alpha_ss > 0.0) {
            any_sess_passes = true;
        }
    }

    ic.passes_stage3_sp500_screen =
        ((res.stats.calmar_ratio > sp.calmar_ratio && ic.jensen_alpha_ann_pct > 0.0) || any_sess_passes);

    if (ic.jensen_alpha_ann_pct > 0.0 && ic.beats_sp500_sharpe &&
        ic.beats_sp500_calmar && ic.beats_sp500_cagr_m_safe) {
        if (ic.badges.find("BEATS_SP500_ALPHA") == std::string::npos) {
            if (!ic.badges.empty()) ic.badges += '|';
            ic.badges += "BEATS_SP500_ALPHA";
        }
    }
}

// ----------------------------------------------------------------------------
// Primary 11-Stage Plug-and-Play Engine Entrypoint
// ----------------------------------------------------------------------------
inline Universal11StageResult run_chunked_parallel_11stage(
    const PrecomputedMarketData& md,
    const StrategyPlugin& plugin,
    const EngineConfig& cfg
) {
    Universal11StageResult res;
    res.config_name = cfg.name;
    res.total_bars = md.n_bars;
    res.total_days = md.total_days;

    if (!plugin.on_bar || md.n_bars == 0 || md.total_days <= 0) {
        res.invariant_ok = false;
        return res;
    }

    if (cfg.assert_invariants) {
        double sll_sum = 0.0;
        for (int s = 0; s < 3; ++s) {
            if (cfg.sll_usd[s] > 0.0) sll_sum += cfg.sll_usd[s];
        }
        if (sll_sum > 0.0 && sll_sum >= cfg.daily_dll_usd) {
            res.invariant_ok = false;
            return res;
        }
        if (cfg.max_fleet_micros > 50 || cfg.max_fleet_micros <= 0 ||
            cfg.contracts_micros <= 0 || cfg.contracts_micros > cfg.max_fleet_micros) {
            res.invariant_ok = false;
            return res;
        }
    }

#ifdef _OPENMP
    double t_start = omp_get_wtime();
#else
    auto t_start = std::clock();
#endif

    // 1. Run Core Overlapping Chunked Simulation (Configured Sizing)
    // Stage 3 Unconstrained uses enforce_prop_limits_on_core (default false) so -$1k DLL / +$1.5k ProfCap never clip runners/stops!
    EngineConfig core_cfg = cfg;
    if (!cfg.enforce_prop_limits_on_core) {
        core_cfg.run_rolling_combines = false; // Combines always run prop-enforced below
    }
    detail::ChunkOutput core = detail::execute_chunked_core(md, plugin, core_cfg);
    res.trades = std::move(core.trades);
    res.days = std::move(core.days);
    if (cfg.enforce_prop_limits_on_core && cfg.run_rolling_combines) {
        res.combine_configured = detail::aggregate_combine_outcomes(core.combines, res.days);
    }

    // 2. Chronological Stitching for Exact Stage 3 Metrics (Configured, Points, 5M, 1M) & Session Breakdown
    double gp = 0.0, gl = 0.0, sum_r = 0.0;
    double pk_cfg = 0.0, eq_pts = 0.0, pk_pts = 0.0;
    double eq_5m = 0.0, pk_5m = 0.0, eq_1m = 0.0, pk_1m = 0.0;
    double sess_gp[4]{}, sess_gl[4]{}, sess_pk[4]{};

    for (const auto& tr : res.trades) {
        res.total_trades++;
        res.net_pnl_configured += tr.net_usd;
        if (res.net_pnl_configured > pk_cfg) pk_cfg = res.net_pnl_configured;
        res.curve_max_dd_configured = std::max(res.curve_max_dd_configured, pk_cfg - res.net_pnl_configured);

        res.net_pts += tr.pts_net;
        eq_pts = res.net_pts;
        if (eq_pts > pk_pts) pk_pts = eq_pts;
        res.curve_max_dd_pts = std::max(res.curve_max_dd_pts, pk_pts - eq_pts);

        double u5 = tr.pts_net * (5.0 * cfg.inst.micro_pv) - 5.0 * cfg.inst.fee_rt_micro;
        eq_5m += u5;
        if (eq_5m > pk_5m) pk_5m = eq_5m;
        res.curve_max_dd_5m = std::max(res.curve_max_dd_5m, pk_5m - eq_5m);

        double u1 = tr.pts_net * cfg.inst.mini_pv - cfg.inst.fee_rt_mini;
        eq_1m += u1;
        if (eq_1m > pk_1m) pk_1m = eq_1m;
        res.curve_max_dd_1m = std::max(res.curve_max_dd_1m, pk_1m - eq_1m);

        sum_r += tr.r_multiple;
        if (tr.sess_id >= 0 && tr.sess_id < 4) {
            int s = tr.sess_id;
            res.sess_trades[s]++;
            res.sess_pnl[s] += tr.net_usd;

            auto& sq = res.sess_quant[s];
            sq.trades++;
            sq.net_pts += tr.pts_net;
            sq.net_usd += tr.net_usd;
            if (sq.net_usd > sess_pk[s]) sess_pk[s] = sq.net_usd;
            sq.curve_max_dd = std::max(sq.curve_max_dd, sess_pk[s] - sq.net_usd);
            if (tr.net_usd > 0.0) { sq.wins++; sess_gp[s] += tr.net_usd; }
            else { sess_gl[s] += std::abs(tr.net_usd); }
        }

        if (tr.net_usd > 0.0) { res.winning_trades++; gp += tr.net_usd; }
        else { gl += std::abs(tr.net_usd); }
    }
    res.net_pnl_5m = eq_5m;
    res.net_pnl_1m = eq_1m;

    // Finalize Per-Session Quant Breakdown
    for (int s = 0; s < 4; ++s) {
        auto& sq = res.sess_quant[s];
        if (sq.trades > 0) {
            sq.win_rate_pct = 100.0 * static_cast<double>(sq.wins) / static_cast<double>(sq.trades);
            sq.profit_factor = (sess_gl[s] > 0.0) ? (sess_gp[s] / sess_gl[s]) : (sess_gp[s] > 0.0 ? 99.0 : 0.0);
            sq.expectancy_pts = sq.net_pts / static_cast<double>(sq.trades);
        }
        double sum_s = 0.0, s2_s = 0.0;
        for (const auto& d : res.days) {
            sum_s += d.sess_pnl[s];
            if (d.hit_dll && d.dll_sess_id == s) sq.dll_hits++;
        }
        if (res.days.size() >= 2) {
            double mean_s = sum_s / static_cast<double>(res.days.size());
            for (const auto& d : res.days) {
                double diff = d.sess_pnl[s] - mean_s;
                s2_s += diff * diff;
            }
            double sd_s = std::sqrt(s2_s / static_cast<double>(res.days.size() - 1));
            sq.sharpe_ann = (sd_s > 1e-12) ? ((mean_s / sd_s) * std::sqrt(252.0)) : 0.0;
        }
    }

    // Bar-by-Bar Real-Time Equity Curve MaxDD (Chronological Stitching of Bar Samples)
    {
        double bar_eq = 0.0, bar_pk = 0.0;
        for (const auto& bs : core.bar_samples) {
            double bar_best = bar_eq + bs.best_intra_delta_in_bar;
            double bar_worst = bar_eq + bs.worst_intra_delta_in_bar;
            if (bar_best > bar_pk) bar_pk = bar_best;
            res.bar_curve_max_dd_configured = std::max(res.bar_curve_max_dd_configured, bar_pk - bar_worst);
            bar_eq += bs.realized_delta_in_bar;
            if (bar_eq > bar_pk) bar_pk = bar_eq;
        }
        double cfg_pv_mult = static_cast<double>(std::max(1, cfg.contracts_micros)) * resolve_pv_per_micro_unit(cfg);
        double scale_5m = (cfg_pv_mult > 1e-12) ? ((5.0 * cfg.inst.micro_pv) / cfg_pv_mult) : 1.0;
        double scale_1m = (cfg_pv_mult > 1e-12) ? (cfg.inst.mini_pv / cfg_pv_mult) : 2.0;
        res.bar_curve_max_dd_5m = res.bar_curve_max_dd_configured * scale_5m;
        res.bar_curve_max_dd_1m = res.bar_curve_max_dd_configured * scale_1m;
    }

    if (res.total_trades > 0) {
        res.win_rate_pct = 100.0 * static_cast<double>(res.winning_trades) / static_cast<double>(res.total_trades);
        res.profit_factor = (gl > 0.0) ? (gp / gl) : 99.0;
        res.trades_per_day = static_cast<double>(res.total_trades) / std::max(1, md.total_days);
        res.expectancy_r = sum_r / static_cast<double>(res.total_trades);
    }

    // Trade Microstructure Metrics
    res.micro = detail::compute_microstructure_metrics(res.trades, res.days);

    // Stage 4: H1 vs H2
    res.h1 = detail::compute_split_metrics(
        res.trades, res.days,
        [&](const TradeRecord& tr) { return tr.cme_yyyymmdd <= cfg.h1_end_yyyymmdd; },
        [&](const DayRecord& d) { return d.cme_yyyymmdd <= cfg.h1_end_yyyymmdd; }
    );
    res.h2 = detail::compute_split_metrics(
        res.trades, res.days,
        [&](const TradeRecord& tr) { return tr.cme_yyyymmdd > cfg.h1_end_yyyymmdd; },
        [&](const DayRecord& d) { return d.cme_yyyymmdd > cfg.h1_end_yyyymmdd; }
    );

    // Multi-Horizon Rolling Lookback Windows (relative to final CME day in dataset)
    if (md.total_days > 0) {
        const int32_t last_d = md.total_days - 1;
        auto calc_win = [&](int32_t lookback_days) -> SplitMetrics {
            int32_t min_d = std::max(0, last_d - lookback_days + 1);
            return detail::compute_split_metrics(
                res.trades, res.days,
                [min_d](const TradeRecord& tr) { return tr.cme_day_idx >= min_d; },
                [min_d](const DayRecord& d) { return d.cme_day_idx >= min_d; }
            );
        };
        res.horizons.w_12m = calc_win(252);
        res.horizons.w_6m  = calc_win(126);
        res.horizons.w_4m  = calc_win(84);
        res.horizons.w_3m  = calc_win(63);
        res.horizons.w_2m  = calc_win(42);
        res.horizons.w_1m  = calc_win(21);
        res.horizons.w_2w  = calc_win(10);
        res.horizons.w_1w  = calc_win(5);
        res.horizons.latest_year = md.day_yyyymmdd.back() / 10000;
        const int32_t ly = res.horizons.latest_year;
        res.horizons.ytd_latest_year = detail::compute_split_metrics(
            res.trades, res.days,
            [ly](const TradeRecord& tr) { return (tr.cme_yyyymmdd / 10000) == ly; },
            [ly](const DayRecord& d) { return (d.cme_yyyymmdd / 10000) == ly; }
        );
    }

    // Auto-Normalized Safe Sizing M_safe
    {
        std::vector<double> intra_dd_per_micro;
        intra_dd_per_micro.reserve(res.days.size());
        const double cfg_m = static_cast<double>(std::max(1, cfg.contracts_micros));
        for (const auto& d : res.days) {
            if (d.trades_cnt > 0 || d.worst_intra_delta < 0.0) {
                intra_dd_per_micro.push_back(std::abs(d.worst_intra_delta) / cfg_m);
            }
        }
        if (!intra_dd_per_micro.empty()) {
            res.p99_daily_intra_dd_per_micro = calc_percentiles(std::move(intra_dd_per_micro)).p99;
        }
        res.m_safe_micros = std::clamp(
            static_cast<int>(std::floor(800.0 / std::max(1e-6, res.p99_daily_intra_dd_per_micro))),
            1, 50
        );
        // Compute unconstrained M_safe curve stats immediately so Stage 3 / S&P 500 screen always has net_pnl_m_safe!
        const bool ms_mini = (res.m_safe_micros >= 10 && (res.m_safe_micros % 10 == 0));
        const double ms_pv = ms_mini ? (cfg.inst.mini_pv / 10.0) * res.m_safe_micros
                                     : cfg.inst.micro_pv * res.m_safe_micros;
        const double ms_fee = resolve_rt_fee_usd(res.m_safe_micros, ms_mini, cfg.inst.fee_rt_micro, cfg.inst.fee_rt_mini);
        double eq_ms = 0.0, pk_ms = 0.0, dd_ms = 0.0;
        for (const auto& tr : res.trades) {
            eq_ms += (tr.pts_net * ms_pv - ms_fee);
            if (eq_ms > pk_ms) pk_ms = eq_ms;
            dd_ms = std::max(dd_ms, pk_ms - eq_ms);
        }
        res.net_pnl_m_safe = eq_ms;
        res.curve_max_dd_m_safe = dd_ms;
        const double cfg_pv_mult_ms = static_cast<double>(std::max(1, cfg.contracts_micros)) * resolve_pv_per_micro_unit(cfg);
        const double scale_ms = (cfg_pv_mult_ms > 1e-12) ? (ms_pv / cfg_pv_mult_ms) : 1.0;
        res.bar_curve_max_dd_m_safe = res.bar_curve_max_dd_configured * scale_ms;
    }

    // Institutional Filters (Outlier Dependency, Long/Short/2022 Bear, Breakeven Slippage Cushion)
    {
        auto& ic = res.inst;
        if (!res.trades.empty()) {
            std::vector<size_t> idx(res.trades.size());
            std::iota(idx.begin(), idx.end(), 0);
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                if (res.trades[a].pts_net != res.trades[b].pts_net)
                    return res.trades[a].pts_net > res.trades[b].pts_net;
                return a < b;
            });
            const size_t k_top = std::min<size_t>(10, idx.size());
            double top10_pts = 0.0, gp_ex10 = 0.0, gl_ex10 = 0.0;
            for (size_t i = 0; i < k_top; ++i) {
                top10_pts += res.trades[idx[i]].pts_net;
            }
            for (size_t i = k_top; i < idx.size(); ++i) {
                const auto& tr = res.trades[idx[i]];
                ic.net_pts_ex_top10 += tr.pts_net;
                if (tr.net_usd > 0.0) gp_ex10 += tr.net_usd;
                else gl_ex10 += std::abs(tr.net_usd);
            }
            ic.pf_ex_top10 = (gl_ex10 > 0.0) ? (gp_ex10 / gl_ex10) : (gp_ex10 > 0.0 ? 99.0 : 0.0);
            if (res.net_pts > 1e-9) {
                ic.top10_trades_pct_of_pnl = 100.0 * top10_pts / res.net_pts;
            } else {
                ic.top10_trades_pct_of_pnl = 0.0;
            }
        }

        // Positive Calendar Years Count
        if (!res.days.empty()) {
            int32_t cur_y = -1;
            double cur_y_pnl = 0.0;
            for (const auto& d : res.days) {
                int32_t y = d.cme_yyyymmdd / 10000;
                if (y != cur_y) {
                    if (cur_y >= 0) {
                        ic.total_years_count++;
                        if (cur_y_pnl > 0.0) ic.positive_years_count++;
                    }
                    cur_y = y;
                    cur_y_pnl = d.day_pnl;
                } else {
                    cur_y_pnl += d.day_pnl;
                }
            }
            if (cur_y >= 0) {
                ic.total_years_count++;
                if (cur_y_pnl > 0.0) ic.positive_years_count++;
            }
        }

        ic.long_split = detail::compute_side_split_metrics(res.trades, res.days, 1);
        ic.short_split = detail::compute_side_split_metrics(res.trades, res.days, -1);
        ic.bear_2022_split = detail::compute_split_metrics(
            res.trades, res.days,
            [](const TradeRecord& tr) { return tr.cme_yyyymmdd >= 20220101 && tr.cme_yyyymmdd <= 20221231; },
            [](const DayRecord& d) { return d.cme_yyyymmdd >= 20220101 && d.cme_yyyymmdd <= 20221231; }
        );
        ic.breakeven_slip_pts = res.micro.expectancy_pts + cfg.inst.entry_slip_pts + cfg.inst.exit_slip_pts;
    }

    // Stage 5: Statistical Rigor
    res.stats = detail::compute_statistical_metrics(res.trades, res.days, res.curve_max_dd_configured, cfg.dsr_num_trials);

    // Stages 6-11 (Prop-Enforced Pipeline: -$1,000 DLL, +$1,500 Cap, $2,000 MLL, 50% Consistency)
    if (!cfg.enforce_prop_limits_on_core && cfg.run_rolling_combines && !cfg.run_full_11_stages) {
        EngineConfig c_prop = cfg;
        c_prop.enforce_prop_limits_on_core = true;
        c_prop.run_rolling_combines = true;
        c_prop.run_full_11_stages = false;
        auto out_p = detail::execute_chunked_core(md, plugin, c_prop);
        res.combine_configured = detail::aggregate_combine_outcomes(out_p.combines, out_p.days);
    }

    if (cfg.run_full_11_stages) {
        auto calc_curve_dds = [&](const detail::ChunkOutput& out_c, double& net_out, double& dd_out, double& bdd_out) {
            double eq = 0.0, pk = 0.0, dd = 0.0;
            for (const auto& tr : out_c.trades) {
                eq += tr.net_usd;
                if (eq > pk) pk = eq;
                dd = std::max(dd, pk - eq);
            }
            double b_eq = 0.0, b_pk = 0.0, b_dd = 0.0;
            for (const auto& bs : out_c.bar_samples) {
                b_pk = std::max(b_pk, b_eq + bs.best_intra_delta_in_bar);
                b_dd = std::max(b_dd, b_pk - (b_eq + bs.worst_intra_delta_in_bar));
                b_eq += bs.realized_delta_in_bar;
                if (b_eq > b_pk) b_pk = b_eq;
            }
            net_out = eq; dd_out = dd; bdd_out = b_dd;
        };

        EngineConfig cfg_prop = cfg;
        cfg_prop.enforce_prop_limits_on_core = true;
        cfg_prop.run_rolling_combines = true;
        cfg_prop.run_full_11_stages = false;

        detail::ChunkOutput out_cfg_prop, out5, out1;
        if (cfg.enforce_prop_limits_on_core && cfg.run_rolling_combines) {
            out_cfg_prop.trades = res.trades;
            out_cfg_prop.days = res.days;
            out_cfg_prop.bar_samples = core.bar_samples;
        } else {
            out_cfg_prop = detail::execute_chunked_core(md, plugin, cfg_prop);
            res.combine_configured = detail::aggregate_combine_outcomes(out_cfg_prop.combines, out_cfg_prop.days);
        }

        // Dual Granular Prop-Enforced Combine Evaluation (5 Micros & 1 Mini)
        if (cfg.contracts_micros == 5 && !cfg.is_mini_mode) {
            res.combine_5m = res.combine_configured;
            out5 = out_cfg_prop;
        } else {
            EngineConfig c5 = cfg_prop;
            c5.contracts_micros = 5;
            c5.is_mini_mode = false;
            out5 = detail::execute_chunked_core(md, plugin, c5);
            res.combine_5m = detail::aggregate_combine_outcomes(out5.combines, out5.days);
        }

        if (cfg.contracts_micros == 10 && cfg.is_mini_mode) {
            res.combine_1m = res.combine_configured;
            out1 = out_cfg_prop;
        } else {
            EngineConfig c1 = cfg_prop;
            c1.contracts_micros = 10;
            c1.is_mini_mode = true;
            out1 = detail::execute_chunked_core(md, plugin, c1);
            res.combine_1m = detail::aggregate_combine_outcomes(out1.combines, out1.days);
        }

        // Stage 8: Funded Policy A & Policy B (Prop-Enforced Configured)
        res.policy_a = detail::simulate_funded_policy(out_cfg_prop.days, false);
        res.policy_b = detail::simulate_funded_policy(out_cfg_prop.days, true);

        // Stage 9: 30,000-Iteration Block-Bootstrap MC (Prop-Enforced Configured)
        res.mc_block_30k = detail::run_block_bootstrap_mc(out_cfg_prop.days, cfg_prop);

        // Stage 10: 30,000-Iteration Noise-Injection Bootstrap (Configured)
        double noise_sr = 0.0;
        res.mc_noise_30k = detail::run_noise_injection_bootstrap(out_cfg_prop.trades, md.total_days, cfg_prop, &noise_sr);
        res.stats.noise_sharpe = noise_sr;
        res.stats.edge_retention_pct = (res.stats.clean_sharpe > 1e-9)
            ? (100.0 * res.stats.noise_sharpe / res.stats.clean_sharpe)
            : 0.0;

        // Auto-Normalized M_safe Prop-Enforced Stages 6-10 Evaluation
        if (res.m_safe_micros == cfg.contracts_micros && !cfg.is_mini_mode) {
            res.combine_m_safe = res.combine_configured;
            res.policy_a_m_safe = res.policy_a;
            res.policy_b_m_safe = res.policy_b;
            res.mc_block_30k_m_safe = res.mc_block_30k;
            res.mc_noise_30k_m_safe = res.mc_noise_30k;
        } else if (res.m_safe_micros == 5) {
            EngineConfig c5_mc = cfg_prop;
            c5_mc.contracts_micros = 5;
            c5_mc.is_mini_mode = false;
            res.combine_m_safe = res.combine_5m;
            res.policy_a_m_safe = detail::simulate_funded_policy(out5.days, false);
            res.policy_b_m_safe = detail::simulate_funded_policy(out5.days, true);
            res.mc_block_30k_m_safe = detail::run_block_bootstrap_mc(out5.days, c5_mc);
            res.mc_noise_30k_m_safe = detail::run_noise_injection_bootstrap(out5.trades, md.total_days, c5_mc);
        } else if (res.m_safe_micros == 10) {
            EngineConfig c1_mc = cfg_prop;
            c1_mc.contracts_micros = 10;
            c1_mc.is_mini_mode = true;
            res.combine_m_safe = res.combine_1m;
            res.policy_a_m_safe = detail::simulate_funded_policy(out1.days, false);
            res.policy_b_m_safe = detail::simulate_funded_policy(out1.days, true);
            res.mc_block_30k_m_safe = detail::run_block_bootstrap_mc(out1.days, c1_mc);
            res.mc_noise_30k_m_safe = detail::run_noise_injection_bootstrap(out1.trades, md.total_days, c1_mc);
        } else {
            EngineConfig cms = cfg_prop;
            cms.contracts_micros = res.m_safe_micros;
            cms.is_mini_mode = (res.m_safe_micros >= 10 && (res.m_safe_micros % 10 == 0));
            auto out_ms = detail::execute_chunked_core(md, plugin, cms);
            res.combine_m_safe = detail::aggregate_combine_outcomes(out_ms.combines, out_ms.days);
            res.policy_a_m_safe = detail::simulate_funded_policy(out_ms.days, false);
            res.policy_b_m_safe = detail::simulate_funded_policy(out_ms.days, true);
            res.mc_block_30k_m_safe = detail::run_block_bootstrap_mc(out_ms.days, cms);
            res.mc_noise_30k_m_safe = detail::run_noise_injection_bootstrap(out_ms.trades, md.total_days, cms);
        }

        // Stage 11: Prop-Enforced (DLL + Cap) Contract Scaling Sweep [2M..50M]
        auto fmt_tier_label = [&](int micros, bool mini) -> std::string {
            double pv_tot = mini ? (static_cast<double>(micros) / 10.0) * cfg.inst.mini_pv
                                 : static_cast<double>(micros) * cfg.inst.micro_pv;
            std::ostringstream oss;
            if (mini && micros >= 10 && micros % 10 == 0) {
                int n_minis = micros / 10;
                oss << micros << "M / " << n_minis << (n_minis == 1 ? " Mini" : " Minis");
            } else {
                oss << micros << "M";
            }
            oss << " ($";
            if (std::abs(pv_tot - std::round(pv_tot)) < 1e-6) {
                oss << static_cast<long long>(std::llround(pv_tot));
            } else {
                oss << std::fixed << std::setprecision(2) << pv_tot;
            }
            oss << "/pt)";
            return oss.str();
        };
        struct TierDef { int micros; bool mini; };
        const TierDef tiers[] = {
            {2,  false},
            {5,  false},
            {10, true},
            {15, false},
            {20, true},
            {30, true},
            {50, true}
        };
        for (const auto& td : tiers) {
            ScalingTierMetrics stm;
            stm.micros = td.micros;
            stm.label = fmt_tier_label(td.micros, td.mini);

            if (td.micros == cfg.contracts_micros && td.mini == cfg.is_mini_mode) {
                calc_curve_dds(out_cfg_prop, stm.net_pnl, stm.curve_max_dd, stm.bar_curve_max_dd);
                stm.combine_pass_rate_pct = res.combine_configured.pass_rate_pct;
                stm.combine_mll_fail_pct = (res.combine_configured.total_trials > 0)
                    ? (100.0 * static_cast<double>(res.combine_configured.fail_mll_cnt) / static_cast<double>(res.combine_configured.total_trials))
                    : 0.0;
                stm.median_ttp = res.combine_configured.ttp.p50;
                stm.p95_intra_dd = res.combine_configured.intra_dd.p95;
            } else if (td.micros == 5 && !td.mini) {
                calc_curve_dds(out5, stm.net_pnl, stm.curve_max_dd, stm.bar_curve_max_dd);
                stm.combine_pass_rate_pct = res.combine_5m.pass_rate_pct;
                stm.combine_mll_fail_pct = (res.combine_5m.total_trials > 0)
                    ? (100.0 * static_cast<double>(res.combine_5m.fail_mll_cnt) / static_cast<double>(res.combine_5m.total_trials))
                    : 0.0;
                stm.median_ttp = res.combine_5m.ttp.p50;
                stm.p95_intra_dd = res.combine_5m.intra_dd.p95;
            } else if (td.micros == 10 && td.mini) {
                calc_curve_dds(out1, stm.net_pnl, stm.curve_max_dd, stm.bar_curve_max_dd);
                stm.combine_pass_rate_pct = res.combine_1m.pass_rate_pct;
                stm.combine_mll_fail_pct = (res.combine_1m.total_trials > 0)
                    ? (100.0 * static_cast<double>(res.combine_1m.fail_mll_cnt) / static_cast<double>(res.combine_1m.total_trials))
                    : 0.0;
                stm.median_ttp = res.combine_1m.ttp.p50;
                stm.p95_intra_dd = res.combine_1m.intra_dd.p95;
            } else {
                EngineConfig ct = cfg_prop;
                ct.contracts_micros = td.micros;
                ct.is_mini_mode = td.mini;
                auto out_t = detail::execute_chunked_core(md, plugin, ct);
                calc_curve_dds(out_t, stm.net_pnl, stm.curve_max_dd, stm.bar_curve_max_dd);
                auto cm = detail::aggregate_combine_outcomes(out_t.combines, out_t.days);
                stm.combine_pass_rate_pct = cm.pass_rate_pct;
                stm.combine_mll_fail_pct = (cm.total_trials > 0) ? (100.0 * cm.fail_mll_cnt / cm.total_trials) : 0.0;
                stm.median_ttp = cm.ttp.p50; stm.p95_intra_dd = cm.intra_dd.p95;
            }
            res.scaling_sweep.push_back(stm);
        }
    }

    classify_strategy(res, cfg);

#ifdef _OPENMP
    res.elapsed_ms = (omp_get_wtime() - t_start) * 1000.0;
#else
    res.elapsed_ms = 1000.0 * static_cast<double>(std::clock() - t_start) / CLOCKS_PER_SEC;
#endif
    return res;
}

// ============================================================================
// STAGE 5: UNIVERSAL PARAMSCHEMA & 3-MODE AUTO-ADAPTIVE FORMULA SWEEPER
// ============================================================================
// Works with ANY strategy formula of ANY complexity (1 to 64+ parameters):
//   - Mode 1 (Always-On): OAT Sensitivity Perturbation (+-10%, +-25%, +-50%)
//   - Mode 2 (K <= 4 & combos <= max_cartesian_combos): Full Cartesian Grid
//   - Mode 3 (K >= 5 or large grid): Deterministic Halton QMC Space-Filling
//   - Post-Sweep: Local +-10% Neighborhood Plateau Audit (ClusterProf% / Local%)
//   - Anti-Overfitting: Exact total_trials_evaluated fed to Stage 6-11 DSR
// ============================================================================
enum class ParamScale : uint8_t {
    LINEAR   = 0,
    LOG      = 1,
    INT      = 2,
    BOOL     = 3,
    DISCRETE = 4
};

struct ParamSpec {
    std::string name;
    double default_val = 0.0;
    double min_val = 0.0;
    double max_val = 0.0;
    int grid_points = 5;
    ParamScale scale = ParamScale::LINEAR;
    std::vector<double> discrete_vals;

    double clamp_and_snap(double v) const {
        if (scale == ParamScale::BOOL) {
            return (v >= 0.5) ? 1.0 : 0.0;
        }
        if (scale == ParamScale::DISCRETE && !discrete_vals.empty()) {
            double best = discrete_vals[0];
            double best_d = std::abs(v - best);
            for (double dv : discrete_vals) {
                double d = std::abs(v - dv);
                if (d < best_d) { best_d = d; best = dv; }
            }
            return best;
        }
        double lo = std::min(min_val, max_val);
        double hi = std::max(min_val, max_val);
        double c = std::clamp(v, lo, hi);
        if (scale == ParamScale::INT) {
            c = std::round(c);
            c = std::clamp(c, std::ceil(lo), std::floor(hi));
        }
        return c;
    }

    double map_unit(double u) const {
        u = std::clamp(u, 0.0, 1.0);
        if (scale == ParamScale::BOOL) {
            return (u >= 0.5) ? 1.0 : 0.0;
        }
        if (scale == ParamScale::DISCRETE && !discrete_vals.empty()) {
            size_t idx = std::min<size_t>(
                discrete_vals.size() - 1,
                static_cast<size_t>(std::floor(u * static_cast<double>(discrete_vals.size())))
            );
            return discrete_vals[idx];
        }
        double lo = std::min(min_val, max_val);
        double hi = std::max(min_val, max_val);
        if (hi - lo <= 1e-12) return clamp_and_snap(default_val);
        if (scale == ParamScale::LOG && lo > 0.0 && hi > 0.0) {
            double log_v = std::log(lo) + u * (std::log(hi) - std::log(lo));
            return std::exp(log_v);
        }
        double val = lo + u * (hi - lo);
        return (scale == ParamScale::INT) ? clamp_and_snap(val) : val;
    }

    std::vector<double> resolve_grid() const {
        std::vector<double> vals;
        if (scale == ParamScale::BOOL) {
            vals = {0.0, 1.0};
        } else if (scale == ParamScale::DISCRETE && !discrete_vals.empty()) {
            vals = discrete_vals;
        } else {
            int n = std::max(2, grid_points);
            for (int i = 0; i < n; ++i) {
                double u = static_cast<double>(i) / static_cast<double>(n - 1);
                vals.push_back(map_unit(u));
            }
        }
        std::sort(vals.begin(), vals.end());
        std::vector<double> uniq;
        for (double v : vals) {
            if (uniq.empty() || std::abs(v - uniq.back()) > 1e-9) {
                uniq.push_back(v);
            }
        }
        if (uniq.empty()) uniq.push_back(default_val);
        return uniq;
    }
};

struct ParamSchema {
    std::vector<ParamSpec> params;

    std::vector<double> defaults() const {
        std::vector<double> d;
        d.reserve(params.size());
        for (const auto& p : params) d.push_back(p.default_val);
        return d;
    }

    size_t cartesian_size(size_t cap = 1000000ULL) const {
        if (params.empty()) return 1;
        size_t prod = 1;
        for (const auto& p : params) {
            size_t g = p.resolve_grid().size();
            if (g == 0) g = 1;
            if (prod > cap / g) return cap + 1;
            prod *= g;
        }
        return prod;
    }
};

using ParameterizedPluginFactory = std::function<StrategyPlugin(const std::vector<double>&, void*)>;

struct Stage5SweepConfig {
    int  max_cartesian_combos = 2000;
    int  high_dim_samples = 1000;
    bool run_oat_sensitivity = true;
    bool run_local_plateau_audit = true;
    int  min_trades_for_champion = 100;
    int  top_k_keep = 10;
};

struct Stage5Candidate {
    std::vector<double> params;
    std::string mode_tag;
    long long trades = 0;
    double win_rate_pct = 0.0;
    double profit_factor = 0.0;
    double net_pts = 0.0;
    double net_pnl_5m = 0.0;
    double curve_max_dd_5m = 0.0;
    double calmar = 0.0;
    double sharpe = 0.0;
    double expectancy_pts = 0.0;
    double breakeven_slip_pts = 0.0;
    bool   passes_sp500_screen = false;
};

struct ParamSensitivity {
    std::string name;
    double default_val = 0.0;
    double best_val = 0.0;
    double default_calmar = 0.0;
    double best_calmar = 0.0;
    double calmar_delta = 0.0;
    double profitable_pct = 0.0;
};

struct Stage5SweepResult {
    std::string sweep_mode_used;
    int    total_trials_evaluated = 0;
    double cluster_prof_pct = 0.0;
    double local_plateau_prof_pct = 0.0;
    bool   plateau_robust = false;
    Stage5Candidate canonical_default;
    Stage5Candidate champion;
    std::vector<Stage5Candidate> top_k;
    std::vector<ParamSensitivity> oat_sensitivity;
    double elapsed_ms = 0.0;
};

namespace detail {

inline double halton_radical_inverse(uint32_t index, uint32_t base) {
    double f = 1.0;
    double r = 0.0;
    while (index > 0) {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(index % base);
        index /= base;
    }
    return r;
}

inline uint32_t nth_prime_u11(size_t dim) {
    static constexpr uint32_t PRIMES[64] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
        59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131,
        137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211, 223,
        227, 229, 233, 239, 241, 251, 257, 263, 269, 271, 277, 281, 283, 293, 307, 311
    };
    return PRIMES[dim % 64];
}

inline Stage5Candidate eval_stage5_point(
    const PrecomputedMarketData& md,
    const ParameterizedPluginFactory& factory,
    void* user_ctx,
    const EngineConfig& base_cfg,
    const SP500Benchmark* sp500,
    const std::vector<double>& pvals,
    const std::string& tag
) {
    StrategyPlugin pl = factory(pvals, user_ctx);
    EngineConfig c = base_cfg;
    c.run_rolling_combines = false;
    c.run_full_11_stages = false;
    c.contracts_micros = 5;
    c.is_mini_mode = false;
    auto r = run_chunked_parallel_11stage(md, pl, c);
    if (sp500 && !sp500->yyyymmdd.empty()) {
        apply_sp500_benchmark(r, *sp500, 50000.0);
    }
    Stage5Candidate cand;
    cand.params = pvals;
    cand.mode_tag = tag;
    cand.trades = r.total_trades;
    cand.win_rate_pct = r.win_rate_pct;
    cand.profit_factor = r.profit_factor;
    cand.net_pts = r.net_pts;
    cand.net_pnl_5m = r.net_pnl_5m;
    cand.curve_max_dd_5m = r.curve_max_dd_5m;
    cand.calmar = r.stats.calmar_ratio;
    cand.sharpe = r.stats.sharpe_ann;
    cand.expectancy_pts = r.micro.expectancy_pts;
    cand.breakeven_slip_pts = r.inst.breakeven_slip_pts;
    cand.passes_sp500_screen = r.inst.passes_stage3_sp500_screen;
    return cand;
}

inline bool stage5_cand_better(const Stage5Candidate& a, const Stage5Candidate& b, int min_trades) {
    bool a_ok = (a.trades >= min_trades && a.profit_factor > 1.0 && a.net_pts > 0.0);
    bool b_ok = (b.trades >= min_trades && b.profit_factor > 1.0 && b.net_pts > 0.0);
    if (a_ok != b_ok) return a_ok > b_ok;
    if (a_ok) {
        if (std::abs(a.calmar - b.calmar) > 1e-12) return a.calmar > b.calmar;
        if (std::abs(a.sharpe - b.sharpe) > 1e-12) return a.sharpe > b.sharpe;
        return a.net_pts > b.net_pts;
    }
    if (std::abs(a.profit_factor - b.profit_factor) > 1e-12) return a.profit_factor > b.profit_factor;
    return a.sharpe > b.sharpe;
}

} // namespace detail

inline Stage5SweepResult run_stage5_universal_formula_sweep(
    const PrecomputedMarketData& md,
    const ParamSchema& schema,
    const ParameterizedPluginFactory& factory,
    void* user_ctx,
    const EngineConfig& base_cfg,
    const Stage5SweepConfig& sw_cfg = Stage5SweepConfig{},
    const SP500Benchmark* sp500 = nullptr
) {
#ifdef _OPENMP
    const double t0 = omp_get_wtime();
#else
    const std::clock_t t0 = std::clock();
#endif
    Stage5SweepResult out;
    const size_t K = schema.params.size();
    const std::vector<double> def_vals = schema.defaults();

    // 0. Canonical Default Baseline
    out.canonical_default = detail::eval_stage5_point(
        md, factory, user_ctx, base_cfg, sp500, def_vals, "DEFAULT"
    );
    std::vector<Stage5Candidate> all_cands;
    all_cands.push_back(out.canonical_default);

    // 1. Mode 1 (Always-On): OAT Sensitivity Perturbation (+-10%, +-25%, +-50%)
    if (sw_cfg.run_oat_sensitivity && K > 0) {
        static constexpr double MULT_STEPS[4] = {0.50, 0.75, 0.90, 1.10};
        static constexpr double MULT_HI[2]    = {1.25, 1.50};
        for (size_t d = 0; d < K; ++d) {
            const auto& ps = schema.params[d];
            std::vector<double> probes;
            if (ps.scale == ParamScale::BOOL || ps.scale == ParamScale::DISCRETE) {
                probes = ps.resolve_grid();
            } else {
                auto add_probe = [&](double v) {
                    double s = ps.clamp_and_snap(v);
                    for (double ex : probes) if (std::abs(ex - s) <= 1e-9) return;
                    if (std::abs(s - ps.default_val) > 1e-9) probes.push_back(s);
                };
                if (std::abs(ps.default_val) > 1e-9) {
                    for (double m : MULT_STEPS) add_probe(ps.default_val * m);
                    for (double m : MULT_HI)    add_probe(ps.default_val * m);
                } else {
                    for (double gv : ps.resolve_grid()) add_probe(gv);
                }
            }

            ParamSensitivity sens;
            sens.name = ps.name;
            sens.default_val = ps.default_val;
            sens.best_val = ps.default_val;
            sens.default_calmar = out.canonical_default.calmar;
            sens.best_calmar = out.canonical_default.calmar;

            int prof_cnt = (out.canonical_default.profit_factor > 1.0 && out.canonical_default.net_pts > 0.0) ? 1 : 0;
            int tested_cnt = 1;
            for (double pv : probes) {
                if (std::abs(pv - ps.default_val) <= 1e-9) continue;
                std::vector<double> vec = def_vals;
                vec[d] = pv;
                auto cand = detail::eval_stage5_point(md, factory, user_ctx, base_cfg, sp500, vec, "OAT");
                all_cands.push_back(cand);
                tested_cnt++;
                if (cand.profit_factor > 1.0 && cand.net_pts > 0.0) prof_cnt++;
                if (cand.calmar > sens.best_calmar) {
                    sens.best_calmar = cand.calmar;
                    sens.best_val = pv;
                }
            }
            sens.calmar_delta = sens.best_calmar - sens.default_calmar;
            sens.profitable_pct = 100.0 * static_cast<double>(prof_cnt) / static_cast<double>(tested_cnt);
            out.oat_sensitivity.push_back(sens);
        }
        std::sort(out.oat_sensitivity.begin(), out.oat_sensitivity.end(),
            [](const ParamSensitivity& a, const ParamSensitivity& b) {
                return std::abs(a.calmar_delta) > std::abs(b.calmar_delta);
            });
    }

    // 2. Auto-Select Mode 2 (Full Cartesian Grid) vs Mode 3 (High-Dim Deterministic Halton QMC)
    if (K > 0) {
        const size_t cart_sz = schema.cartesian_size(static_cast<size_t>(sw_cfg.max_cartesian_combos) + 1ULL);
        if (K <= 4 && cart_sz <= static_cast<size_t>(sw_cfg.max_cartesian_combos)) {
            out.sweep_mode_used = "CARTESIAN";
            std::vector<std::vector<double>> grids(K);
            for (size_t d = 0; d < K; ++d) grids[d] = schema.params[d].resolve_grid();

            std::vector<size_t> idx(K, 0);
            while (true) {
                std::vector<double> vec(K);
                for (size_t d = 0; d < K; ++d) vec[d] = grids[d][idx[d]];
                auto cand = detail::eval_stage5_point(md, factory, user_ctx, base_cfg, sp500, vec, "CARTESIAN");
                all_cands.push_back(std::move(cand));

                size_t pos = K;
                while (pos > 0) {
                    --pos;
                    idx[pos]++;
                    if (idx[pos] < grids[pos].size()) break;
                    idx[pos] = 0;
                    if (pos == 0) goto cart_done;
                }
            }
        cart_done:;
        } else {
            out.sweep_mode_used = "HIGH_DIM_QMC_LHS";
            const int n_qmc = std::max(1, sw_cfg.high_dim_samples);
            for (int i = 1; i <= n_qmc; ++i) {
                std::vector<double> vec(K);
                for (size_t d = 0; d < K; ++d) {
                    double u = detail::halton_radical_inverse(
                        static_cast<uint32_t>(i),
                        detail::nth_prime_u11(d)
                    );
                    vec[d] = schema.params[d].map_unit(u);
                }
                auto cand = detail::eval_stage5_point(md, factory, user_ctx, base_cfg, sp500, vec, "QMC_LHS");
                all_cands.push_back(std::move(cand));
            }
        }
    } else {
        out.sweep_mode_used = "ZERO_PARAM_CANONICAL";
    }

    // 3. Rank Candidates & Compute Global Cluster Proficiency %
    std::sort(all_cands.begin(), all_cands.end(),
        [&](const Stage5Candidate& a, const Stage5Candidate& b) {
            return detail::stage5_cand_better(a, b, sw_cfg.min_trades_for_champion);
        });

    size_t prof_global = 0;
    for (const auto& c : all_cands) {
        if (c.trades >= sw_cfg.min_trades_for_champion && c.profit_factor > 1.0 && c.net_pts > 0.0) {
            prof_global++;
        }
    }
    out.total_trials_evaluated = static_cast<int>(all_cands.size());
    out.cluster_prof_pct = all_cands.empty()
        ? 0.0
        : (100.0 * static_cast<double>(prof_global) / static_cast<double>(all_cands.size()));
    out.champion = all_cands.empty() ? out.canonical_default : all_cands[0];

    const size_t k_keep = std::min<size_t>(static_cast<size_t>(std::max(1, sw_cfg.top_k_keep)), all_cands.size());
    out.top_k.assign(all_cands.begin(), all_cands.begin() + static_cast<std::ptrdiff_t>(k_keep));

    // 4. Local +-10% Neighborhood Plateau Audit around Champion
    if (sw_cfg.run_local_plateau_audit && K > 0) {
        int nb_tested = 0;
        int nb_prof = 0;
        for (size_t d = 0; d < K; ++d) {
            const auto& ps = schema.params[d];
            if (ps.scale == ParamScale::BOOL) continue;
            double base_v = out.champion.params[d];
            double deltas[2] = {0.90, 1.10};
            for (double m : deltas) {
                double nv = (std::abs(base_v) > 1e-9)
                    ? ps.clamp_and_snap(base_v * m)
                    : ps.clamp_and_snap(base_v + (m - 1.0) * (ps.max_val - ps.min_val));
                if (std::abs(nv - base_v) <= 1e-9) continue;
                std::vector<double> vec = out.champion.params;
                vec[d] = nv;
                auto cand = detail::eval_stage5_point(md, factory, user_ctx, base_cfg, sp500, vec, "PLATEAU");
                nb_tested++;
                out.total_trials_evaluated++;
                if (cand.trades >= sw_cfg.min_trades_for_champion && cand.profit_factor > 1.0 && cand.net_pts > 0.0) {
                    nb_prof++;
                }
            }
        }
        out.local_plateau_prof_pct = (nb_tested > 0)
            ? (100.0 * static_cast<double>(nb_prof) / static_cast<double>(nb_tested))
            : ((out.champion.profit_factor > 1.0 && out.champion.net_pts > 0.0) ? 100.0 : 0.0);
        out.plateau_robust = (out.local_plateau_prof_pct >= 60.0) || (out.cluster_prof_pct >= 25.0);
    }

#ifdef _OPENMP
    out.elapsed_ms = (omp_get_wtime() - t0) * 1000.0;
#else
    out.elapsed_ms = 1000.0 * static_cast<double>(std::clock() - t0) / CLOCKS_PER_SEC;
#endif
    return out;
}

} // namespace u11
