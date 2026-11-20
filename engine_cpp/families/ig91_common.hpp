#pragma once
#include "strategy_registry_546.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace s546 {
namespace ig91 {

// ----------------------------------------------------------------------------
// Symbol-Agnostic Parameter & Signal Helpers
// ----------------------------------------------------------------------------
inline double p_at(const std::vector<double>& p, size_t idx, double def_val) {
    return (idx < p.size()) ? p[idx] : def_val;
}

inline double sym_pt_scale(const u11::InstrumentSpec& inst) {
    return 2.0 / std::max(0.1, inst.micro_pv);
}

inline double clamp_sl(double raw_sl, const u11::InstrumentSpec& inst) {
    return std::max(2.0 * inst.tick_size, raw_sl);
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

inline int ymd_dow(int32_t ymd) {
    int y = ymd / 10000;
    int m = (ymd / 100) % 100;
    int d = ymd % 100;
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

inline int32_t ymd_to_days(int32_t ymd) {
    int y = ymd / 10000;
    int m = (ymd / 100) % 100;
    int d = ymd % 100;
    if (m <= 2) { y -= 1; m += 12; }
    return 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d;
}

// ----------------------------------------------------------------------------
// Causal Multi-Timeframe Bar Aggregator (Zero Lookahead)
// Emits `just_closed == true` on the first 1m bar of the NEW bucket,
// exposing `closed` (the completed prior bucket's OHLCV).
// ----------------------------------------------------------------------------
struct AggBar {
    double o = 0.0, h = 0.0, l = 0.0, c = 0.0, v = 0.0;
    int64_t bucket = -1;
    bool valid = false;
};

struct BarAggregator {
    int tf_mins = 15;
    AggBar cur{};
    AggBar closed{};
    std::deque<AggBar> history; // up to max_hist completed bars (index 0 = most recent closed)
    size_t max_hist = 64;
    bool just_closed = false;

    void reset(int mins, size_t hist_cap = 64) {
        tf_mins = std::max(1, mins);
        max_hist = hist_cap;
        cur = AggBar{};
        closed = AggBar{};
        history.clear();
        just_closed = false;
    }

    void update(const u11::CausalBarView& view) {
        just_closed = false;
        int32_t ymd = view.day_yyyymmdd();
        int16_t em = view.et_mins();
        int64_t b = (tf_mins >= 1440)
            ? static_cast<int64_t>(ymd)
            : (static_cast<int64_t>(ymd) * 1440LL + static_cast<int64_t>(em / tf_mins));

        double o = static_cast<double>(view[0].o);
        double h = static_cast<double>(view[0].h);
        double l = static_cast<double>(view[0].l);
        double c = static_cast<double>(view[0].c);
        double vol = static_cast<double>(view[0].v);

        if (cur.valid && b != cur.bucket) {
            closed = cur;
            history.push_front(closed);
            if (history.size() > max_hist) history.pop_back();
            just_closed = true;
            cur = AggBar{o, h, l, c, vol, b, true};
        } else if (!cur.valid) {
            cur = AggBar{o, h, l, c, vol, b, true};
        } else {
            cur.h = std::max(cur.h, h);
            cur.l = std::min(cur.l, l);
            cur.c = c;
            cur.v += vol;
        }
    }

    const AggBar& bar(size_t lag = 0) const {
        static const AggBar empty{};
        return (lag < history.size()) ? history[lag] : empty;
    }
    size_t size() const { return history.size(); }
};

// ----------------------------------------------------------------------------
// Causal FVG / IFVG / BPR Tracker
// ----------------------------------------------------------------------------
struct FvgZone {
    int8_t dir = 0;         // +1 bullish gap, -1 bearish gap
    double lo = 0.0;        // lower bound
    double hi = 0.0;        // upper bound
    int age = 0;            // bars since formation
    bool inverted = false;  // true once closed through -> IFVG (dir flipped!)
    bool mitigated = false; // true once tapped after formation/inversion
};

struct FvgTracker {
    std::deque<FvgZone> zones;
    size_t max_zones = 12;
    bool has_bpr = false;
    double bpr_lo = 0.0;
    double bpr_hi = 0.0;
    int8_t bpr_dir = 0;

    void reset(size_t cap = 12) {
        zones.clear();
        max_zones = cap;
        has_bpr = false;
    }

    // Feed 3 consecutive completed bars: b0 (newest), b1 (mid), b2 (oldest)
    void on_closed_bar(const AggBar& b0, const AggBar& b1, const AggBar& b2, double min_gap_pts) {
        for (auto& z : zones) z.age++;

        // Check inversion & mitigation on existing zones using b0
        for (auto& z : zones) {
            if (!z.inverted) {
                if (z.dir == 1 && b0.c < z.lo) {
                    z.inverted = true;
                    z.dir = -1;
                    z.age = 0;
                    z.mitigated = false;
                } else if (z.dir == -1 && b0.c > z.hi) {
                    z.inverted = true;
                    z.dir = 1;
                    z.age = 0;
                    z.mitigated = false;
                }
            }
        }

        // Detect new FVG from b2, b1, b0
        if (b2.valid && b1.valid && b0.valid) {
            if (b0.l > b2.h + min_gap_pts) {
                FvgZone nz{1, b2.h, b0.l, 0, false, false};
                // Check BPR overlap with recent bearish FVG
                for (const auto& oz : zones) {
                    if (oz.dir == -1 && !oz.inverted && oz.age <= 30) {
                        double olo = std::max(nz.lo, oz.lo);
                        double ohi = std::min(nz.hi, oz.hi);
                        if (ohi > olo) {
                            has_bpr = true;
                            bpr_lo = olo;
                            bpr_hi = ohi;
                            bpr_dir = 1;
                        }
                    }
                }
                zones.push_front(nz);
            } else if (b0.h < b2.l - min_gap_pts) {
                FvgZone nz{-1, b0.h, b2.l, 0, false, false};
                for (const auto& oz : zones) {
                    if (oz.dir == 1 && !oz.inverted && oz.age <= 30) {
                        double olo = std::max(nz.lo, oz.lo);
                        double ohi = std::min(nz.hi, oz.hi);
                        if (ohi > olo) {
                            has_bpr = true;
                            bpr_lo = olo;
                            bpr_hi = ohi;
                            bpr_dir = -1;
                        }
                    }
                }
                zones.push_front(nz);
            }
            while (zones.size() > max_zones) zones.pop_back();
        }
    }

    // Check if price `px` (or [lo, hi]) taps any active FVG (or IFVG if `want_inverted`)
    bool find_tap(double lo, double hi, int8_t dir, bool want_inverted, int max_age, FvgZone& out) {
        for (auto& z : zones) {
            if (z.dir == dir && z.inverted == want_inverted && z.age <= max_age && !z.mitigated) {
                if (hi >= z.lo && lo <= z.hi) {
                    z.mitigated = true;
                    out = z;
                    return true;
                }
            }
        }
        return false;
    }
};

// ----------------------------------------------------------------------------
// Causal Swing / Pivot / BOS / CISD Tracker
// ----------------------------------------------------------------------------
struct SwingTracker {
    int k = 3; // left & right bars (confirmed causally at t when pivot was at t-k)
    std::deque<AggBar> win;
    double last_sh = 0.0, prev_sh = 0.0;
    double last_sl = 0.0, prev_sl = 0.0;
    bool has_sh = false, has_sl = false;
    int8_t trend_dir = 0; // +1 bullish BOS, -1 bearish BOS
    bool bos_up_now = false, bos_dn_now = false;
    bool sweep_hi_now = false, sweep_lo_now = false;

    void reset(int pivot_k = 3) {
        k = std::max(1, pivot_k);
        win.clear();
        last_sh = prev_sh = last_sl = prev_sl = 0.0;
        has_sh = has_sl = false;
        trend_dir = 0;
        bos_up_now = bos_dn_now = sweep_hi_now = sweep_lo_now = false;
    }

    void update(const AggBar& b) {
        bos_up_now = bos_dn_now = sweep_hi_now = sweep_lo_now = false;
        if (!b.valid) return;

        // Check sweeps & BOS against already-confirmed swings BEFORE updating
        if (has_sh) {
            if (b.h > last_sh && b.c <= last_sh) sweep_hi_now = true;
            if (b.c > last_sh) { bos_up_now = true; trend_dir = 1; }
        }
        if (has_sl) {
            if (b.l < last_sl && b.c >= last_sl) sweep_lo_now = true;
            if (b.c < last_sl) { bos_dn_now = true; trend_dir = -1; }
        }

        win.push_back(b);
        size_t need = static_cast<size_t>(2 * k + 1);
        if (win.size() > need) win.pop_front();
        if (win.size() == need) {
            const double cand_h = win[static_cast<size_t>(k)].h;
            const double cand_l = win[static_cast<size_t>(k)].l;
            bool is_sh = true, is_sl = true;
            for (size_t i = 0; i < need; ++i) {
                if (i == static_cast<size_t>(k)) continue;
                if (win[i].h >= cand_h) is_sh = false;
                if (win[i].l <= cand_l) is_sl = false;
            }
            if (is_sh) {
                prev_sh = has_sh ? last_sh : cand_h;
                last_sh = cand_h;
                has_sh = true;
            }
            if (is_sl) {
                prev_sl = has_sl ? last_sl : cand_l;
                last_sl = cand_l;
                has_sl = true;
            }
        }
    }
};

// ----------------------------------------------------------------------------
// Causal Volume Profile Tracker (POC / VAH / VAL / LVN / HVN)
// ----------------------------------------------------------------------------
struct VolumeProfileTracker {
    double bin_pts = 5.0;
    std::map<int64_t, double> bins;
    double total_vol = 0.0;
    double poc = 0.0, vah = 0.0, val = 0.0;
    double mean_bin_vol = 0.0;
    bool valid = false;

    void reset(double bpts = 5.0) {
        bin_pts = std::max(0.25, bpts);
        bins.clear();
        total_vol = 0.0;
        poc = vah = val = mean_bin_vol = 0.0;
        valid = false;
    }

    void add_bar(double h, double l, double c, double vol) {
        double typ = (h + l + c) / 3.0;
        int64_t key = static_cast<int64_t>(std::llround(typ / bin_pts));
        double v = std::max(1.0, vol);
        bins[key] += v;
        total_vol += v;
    }

    void compute() {
        if (bins.size() < 4 || total_vol <= 0.0) { valid = false; return; }
        int64_t best_k = 0;
        double best_v = -1.0;
        for (const auto& kv : bins) {
            if (kv.second > best_v) { best_v = kv.second; best_k = kv.first; }
        }
        poc = static_cast<double>(best_k) * bin_pts;
        mean_bin_vol = total_vol / static_cast<double>(bins.size());

        // Expand Value Area (70% of total_vol) around POC
        auto it_l = bins.find(best_k);
        auto it_r = it_l;
        double va_vol = best_v;
        const double target = 0.70 * total_vol;
        int64_t lo_k = best_k, hi_k = best_k;

        while (va_vol < target) {
            bool can_l = (it_l != bins.begin());
            auto next_r = std::next(it_r);
            bool can_r = (next_r != bins.end());
            if (!can_l && !can_r) break;
            double vl = can_l ? std::prev(it_l)->second : -1.0;
            double vr = can_r ? next_r->second : -1.0;
            if (vr >= vl && can_r) {
                it_r = next_r;
                hi_k = it_r->first;
                va_vol += it_r->second;
            } else if (can_l) {
                --it_l;
                lo_k = it_l->first;
                va_vol += it_l->second;
            }
        }
        val = static_cast<double>(lo_k) * bin_pts;
        vah = static_cast<double>(hi_k) * bin_pts;
        valid = (vah > val);
    }

    double bin_vol_at(double px) const {
        if (bins.empty()) return 0.0;
        int64_t key = static_cast<int64_t>(std::llround(px / bin_pts));
        auto it = bins.find(key);
        return (it != bins.end()) ? it->second : 0.0;
    }

    bool is_lvn(double px, double lvn_ratio = 0.65) const {
        if (!valid || mean_bin_vol <= 0.0) return false;
        return bin_vol_at(px) <= lvn_ratio * mean_bin_vol;
    }

    bool is_hvn(double px, double hvn_ratio = 1.35) const {
        if (!valid || mean_bin_vol <= 0.0) return false;
        return bin_vol_at(px) >= hvn_ratio * mean_bin_vol;
    }
};

// ----------------------------------------------------------------------------
// Causal Session & Key-Time Range Tracker
// ----------------------------------------------------------------------------
struct SessionLevelsTracker {
    int32_t cur_day = -1;
    // Asia: 18:00-23:59 ET
    double asia_h = -1e18, asia_l = 1e18; bool asia_ok = false;
    // London: 00:00-08:29 ET
    double lon_h = -1e18, lon_l = 1e18; bool lon_ok = false;
    // True Day Open (00:00 ET) & 00:00-00:30 range
    double tdo_open = 0.0, tdo30_h = -1e18, tdo30_l = 1e18;
    bool has_tdo = false, tdo30_ok = false;
    // 8:30 AM ET open
    double open_830 = 0.0; bool has_830 = false;
    // 8:00-8:59 AM 1H candle
    double h8_h = -1e18, h8_l = 1e18; bool h8_ok = false;
    // 9:00-9:14 AM / 9:00-9:29 AM
    double h9_h = -1e18, h9_l = 1e18; bool h9_ok = false;
    // 8:12-9:12 ET premarket range
    double pm812_h = -1e18, pm812_l = 1e18; bool pm812_ok = false;
    // NY PM ORB (13:30-14:00 ET)
    double pm_orb_h = -1e18, pm_orb_l = 1e18; bool pm_orb_ok = false;
    // NY Final Hours (14:00-16:00 ET) from PRIOR day
    double prev_ny_late_h = 0.0, prev_ny_late_l = 0.0; bool has_prev_ny_late = false;
    double cur_ny_late_h = -1e18, cur_ny_late_l = 1e18; bool cur_ny_late_ok = false;
    // Intraday RTH (09:30+) running H/L
    double rth_h = -1e18, rth_l = 1e18; bool rth_ok = false;

    void update(const u11::CausalBarView& v) {
        int32_t ymd = v.day_yyyymmdd();
        int16_t em = v.et_mins();
        double o = static_cast<double>(v[0].o);
        double h = static_cast<double>(v[0].h);
        double l = static_cast<double>(v[0].l);

        if (ymd != cur_day) {
            if (cur_ny_late_ok) {
                prev_ny_late_h = cur_ny_late_h;
                prev_ny_late_l = cur_ny_late_l;
                has_prev_ny_late = true;
            }
            cur_day = ymd;
            asia_h = -1e18; asia_l = 1e18; asia_ok = false;
            lon_h = -1e18; lon_l = 1e18; lon_ok = false;
            tdo_open = 0.0; tdo30_h = -1e18; tdo30_l = 1e18; has_tdo = false; tdo30_ok = false;
            open_830 = 0.0; has_830 = false;
            h8_h = -1e18; h8_l = 1e18; h8_ok = false;
            h9_h = -1e18; h9_l = 1e18; h9_ok = false;
            pm812_h = -1e18; pm812_l = 1e18; pm812_ok = false;
            pm_orb_h = -1e18; pm_orb_l = 1e18; pm_orb_ok = false;
            cur_ny_late_h = -1e18; cur_ny_late_l = 1e18; cur_ny_late_ok = false;
            rth_h = -1e18; rth_l = 1e18; rth_ok = false;
        }

        if (em >= 1080) { // 18:00 - 23:59 ET
            asia_h = std::max(asia_h, h);
            asia_l = std::min(asia_l, l);
            asia_ok = true;
        }
        if (em >= 0 && em < 510) { // 00:00 - 08:29 ET
            lon_h = std::max(lon_h, h);
            lon_l = std::min(lon_l, l);
            lon_ok = true;
        }
        if (em >= 0 && em < 30) {
            if (!has_tdo) { tdo_open = o; has_tdo = true; }
            tdo30_h = std::max(tdo30_h, h);
            tdo30_l = std::min(tdo30_l, l);
            tdo30_ok = true;
        }
        if (em >= 510 && !has_830) {
            open_830 = o;
            has_830 = true;
        }
        if (em >= 480 && em < 540) { // 08:00 - 08:59 ET
            h8_h = std::max(h8_h, h);
            h8_l = std::min(h8_l, l);
            h8_ok = true;
        }
        if (em >= 540 && em < 570) { // 09:00 - 09:29 ET
            h9_h = std::max(h9_h, h);
            h9_l = std::min(h9_l, l);
            h9_ok = true;
        }
        if (em >= 492 && em < 552) { // 08:12 - 09:12 ET
            pm812_h = std::max(pm812_h, h);
            pm812_l = std::min(pm812_l, l);
            pm812_ok = true;
        }
        if (em >= 810 && em < 840) { // 13:30 - 14:00 ET
            pm_orb_h = std::max(pm_orb_h, h);
            pm_orb_l = std::min(pm_orb_l, l);
            pm_orb_ok = true;
        }
        if (em >= 840 && em < 960) { // 14:00 - 16:00 ET
            cur_ny_late_h = std::max(cur_ny_late_h, h);
            cur_ny_late_l = std::min(cur_ny_late_l, l);
            cur_ny_late_ok = true;
        }
        if (em >= 570 && em < 960) {
            rth_h = std::max(rth_h, h);
            rth_l = std::min(rth_l, l);
            rth_ok = true;
        }
    }
};

// ----------------------------------------------------------------------------
// Causal Past-Bar Pre-Open Scanner (07:30-09:29 ET has sess_id == -1 in engine)
// Called once at NY_AM open (em >= 570) using strictly causal lag <= -1!
// ----------------------------------------------------------------------------
struct PreopenLevels {
    double h8_h = -1e18, h8_l = 1e18; bool h8_ok = false;
    double h9_h = -1e18, h9_l = 1e18; bool h9_ok = false;
    double pm812_h = -1e18, pm812_l = 1e18; bool pm812_ok = false;
    double ce50_hi = -1e18, ce50_lo = 1e18; bool ce50_ok = false;
    double pre_news_px = 0.0;
    int8_t news_dir = 0;
    int8_t h8_swept_dir = 0;
    int8_t h9_swept_dir = 0;
    int8_t sweep89_dir = 0;
    int8_t pm812_swept_dir = 0;
};

inline PreopenLevels scan_preopen_levels(const u11::CausalBarView& v, double atr) {
    PreopenLevels out{};
    if (v.bar_index() < 10) return out;
    int32_t cur_cme = v.cme_day_idx();
    int max_back = static_cast<int>(std::min<size_t>(v.bar_index(), 130));
    double post_news_hi = -1e18, post_news_lo = 1e18;
    bool has_pre_news = false;

    for (int lag = -max_back; lag <= -1; ++lag) {
        size_t cidx = v.causal_idx(lag);
        if (v.md->meta[cidx].cme_day_idx != cur_cme) continue;
        int16_t em = v.md->meta[cidx].et_min;
        double bh = static_cast<double>(v[lag].h);
        double bl = static_cast<double>(v[lag].l);
        double bc = static_cast<double>(v[lag].c);

        if (em >= 480 && em < 540) {
            out.h8_h = std::max(out.h8_h, bh);
            out.h8_l = std::min(out.h8_l, bl);
            out.h8_ok = (out.h8_h > out.h8_l);
        }
        if (em >= 540 && em < 570) {
            out.h9_h = std::max(out.h9_h, bh);
            out.h9_l = std::min(out.h9_l, bl);
            out.h9_ok = (out.h9_h > out.h9_l);
            if (out.h8_ok) {
                if (bl < out.h8_l) out.h8_swept_dir = 1;
                else if (bh > out.h8_h) out.h8_swept_dir = -1;
            }
        }
        if (em >= 492 && em < 552) {
            out.pm812_h = std::max(out.pm812_h, bh);
            out.pm812_l = std::min(out.pm812_l, bl);
            out.pm812_ok = (out.pm812_h > out.pm812_l);
        }
        if (em >= 552 && em < 570 && out.pm812_ok) {
            if (bl < out.pm812_l) out.pm812_swept_dir = 1;
            else if (bh > out.pm812_h) out.pm812_swept_dir = -1;
        }
        if (em >= 510 && em < 570) {
            out.ce50_hi = std::max(out.ce50_hi, bh);
            out.ce50_lo = std::min(out.ce50_lo, bl);
            out.ce50_ok = (out.ce50_hi > out.ce50_lo);
            post_news_hi = std::max(post_news_hi, bh);
            post_news_lo = std::min(post_news_lo, bl);
        }
        if (em >= 500 && em < 510) {
            out.pre_news_px = bc;
            has_pre_news = true;
        }
    }

    if (has_pre_news && out.ce50_ok) {
        double up_mv = post_news_hi - out.pre_news_px;
        double dn_mv = out.pre_news_px - post_news_lo;
        if (up_mv >= atr * 1.0 && up_mv >= dn_mv) out.news_dir = 1;
        else if (dn_mv >= atr * 1.0 && dn_mv > up_mv) out.news_dir = -1;
    }
    if (out.h8_swept_dir != 0) out.sweep89_dir = out.h8_swept_dir;
    return out;
}

// ----------------------------------------------------------------------------
// Causal Indicator Pack (RSI, BB, EMA100/200, Williams %R, CVD Proxy, IV Wall)
// ----------------------------------------------------------------------------
struct IndicatorPack {
    double ema100 = 0.0, ema200 = 0.0, kama = 0.0;
    double rsi14 = 50.0, prev_rsi14 = 50.0;
    double avg_gain = 0.0, avg_loss = 0.0;
    double prev_c = 0.0;
    double bb_mid = 0.0, bb_up = 0.0, bb_dn = 0.0;
    double williams_r = -50.0;
    double stoch_k = 50.0, stoch_d = 50.0;
    double cvd = 0.0, prev_cvd = 0.0;
    double rv_wall_hi = 0.0, rv_wall_lo = 0.0;
    double day_open = 0.0;
    int32_t cur_day = -1;
    int count = 0;
    s546::RollingWindow win_c{20};
    s546::RollingWindow win_h{14};
    s546::RollingWindow win_l{14};
    s546::RollingWindow win_ret{20};
    s546::RollingWindow win_k{3};

    void reset() {
        ema100 = ema200 = kama = 0.0;
        rsi14 = prev_rsi14 = 50.0;
        avg_gain = avg_loss = prev_c = 0.0;
        bb_mid = bb_up = bb_dn = 0.0;
        williams_r = -50.0;
        stoch_k = stoch_d = 50.0;
        cvd = prev_cvd = 0.0;
        rv_wall_hi = rv_wall_lo = day_open = 0.0;
        cur_day = -1;
        count = 0;
        win_c.reset(20);
        win_h.reset(14);
        win_l.reset(14);
        win_ret.reset(20);
        win_k.reset(3);
    }

    void update(const u11::CausalBarView& v, const u11::InstrumentSpec& inst) {
        double o = static_cast<double>(v[0].o);
        double h = static_cast<double>(v[0].h);
        double l = static_cast<double>(v[0].l);
        double c = static_cast<double>(v[0].c);
        double vol = static_cast<double>(v[0].v);
        int32_t ymd = v.day_yyyymmdd();

        if (ymd != cur_day) {
            cur_day = ymd;
            day_open = o;
            cvd = 0.0;
            prev_cvd = 0.0;
            double sig_d = std::max(inst.tick_size * 4.0, win_ret.stdev() * std::sqrt(390.0));
            rv_wall_hi = day_open + sig_d;
            rv_wall_lo = day_open - sig_d;
        }

        if (count == 0) {
            ema100 = ema200 = kama = c;
            prev_c = c;
        } else {
            ema100 += (2.0 / 101.0) * (c - ema100);
            ema200 += (2.0 / 201.0) * (c - ema200);
            double sc = 0.15;
            kama += sc * (c - kama);

            double chg = c - prev_c;
            double g = chg > 0.0 ? chg : 0.0;
            double ls = chg < 0.0 ? -chg : 0.0;
            if (count <= 14) {
                avg_gain += g / 14.0;
                avg_loss += ls / 14.0;
            } else {
                avg_gain = (avg_gain * 13.0 + g) / 14.0;
                avg_loss = (avg_loss * 13.0 + ls) / 14.0;
            }
            prev_rsi14 = rsi14;
            if (avg_loss > 1e-9) {
                double rs = avg_gain / avg_loss;
                rsi14 = 100.0 - 100.0 / (1.0 + rs);
            } else {
                rsi14 = (avg_gain > 0.0) ? 100.0 : 50.0;
            }
            win_ret.push(chg);
            prev_c = c;
        }

        win_c.push(c);
        win_h.push(h);
        win_l.push(l);
        if (win_c.count >= 2) {
            bb_mid = win_c.mean();
            double sd = win_c.stdev();
            bb_up = bb_mid + 2.0 * sd;
            bb_dn = bb_mid - 2.0 * sd;
        }
        double hh = win_h.max_val();
        double ll = win_l.min_val();
        if (hh > ll) {
            williams_r = -100.0 * (hh - c) / (hh - ll);
            double k_raw = 100.0 * (c - ll) / (hh - ll);
            win_k.push(k_raw);
            stoch_k = k_raw;
            stoch_d = win_k.mean();
        }
        prev_cvd = cvd;
        double rng = std::max(inst.tick_size, h - l);
        cvd += vol * ((c - o) / rng);
        ++count;
    }
};

// ----------------------------------------------------------------------------
// Reels #059 & #060: Reusable LVN Pass-Through + Wick Aggression Regime Filter
// ----------------------------------------------------------------------------
struct LvnRegimeFilter {
    static bool allows(
        const VolumeProfileTracker& vp,
        const u11::CausalBarView& v,
        int8_t dir,
        double lvn_ratio = 0.85,
        double wick_min = 0.20,
        double rvol_min = 0.80
    ) {
        if (dir == 0) return false;
        double c = static_cast<double>(v[0].c);
        double o = static_cast<double>(v[0].o);
        double h = static_cast<double>(v[0].h);
        double l = static_cast<double>(v[0].l);
        double rng = std::max(1e-6, h - l);

        // #059: Require LVN pass-through zone (or outside VA) rather than HVN chop
        if (vp.valid && !vp.is_lvn(c, lvn_ratio) && vp.is_hvn(c, 1.35)) {
            return false;
        }
        // #060: Wick rejection + volume aggression alignment
        if (v.rel_vol() < rvol_min) return false;
        if (dir == 1) {
            double lower_wick = (std::min(o, c) - l) / rng;
            if (lower_wick < wick_min && c < o) return false;
        } else {
            double upper_wick = (h - std::max(o, c)) / rng;
            if (upper_wick < wick_min && c > o) return false;
        }
        return true;
    }
};

} // namespace ig91
} // namespace s546
