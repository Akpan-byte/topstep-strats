#pragma once

#include "universal_11stage_plug_and_play_engine.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace w7 {

using namespace u11;

static constexpr double PI_VAL = 3.14159265358979323846;
static constexpr size_t MAX_LEGS = 128;

inline InstrumentSpec realistic_spec(const std::string& sym) {
    InstrumentSpec sp = get_instrument_spec(sym);
    if (sym == "NQ" || sym == "MNQ") {
        sp.entry_slip_pts = 0.50;
        sp.exit_slip_pts = 0.50;
    } else if (sym == "ES" || sym == "MES") {
        sp.entry_slip_pts = 0.25;
        sp.exit_slip_pts = 0.25;
    } else if (sym == "YM" || sym == "MYM") {
        sp.entry_slip_pts = 1.00;
        sp.exit_slip_pts = 1.00;
    }
    return sp;
}

inline InstrumentSpec pessimistic_spec(const std::string& sym) {
    return get_instrument_spec(sym);
}

inline int8_t sym_to_id(const std::string& sym) {
    if (sym == "NQ" || sym == "MNQ") return 0;
    if (sym == "ES" || sym == "MES") return 1;
    return 2;
}

inline const char* id_to_sym(int8_t sid) {
    if (sid == 0) return "NQ";
    if (sid == 1) return "ES";
    return "YM";
}

inline const char* sess_name(int8_t s) {
    if (s == 0) return "Asian";
    if (s == 1) return "London";
    if (s == 2) return "NY_AM";
    return "NY_PM";
}

inline int ymd_to_dow04(int32_t ymd) {
    int y = ymd / 10000, m = (ymd / 100) % 100, d = ymd % 100;
    static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return std::clamp(w - 1, 0, 4);
}

inline int ymd_to_dom(int32_t ymd) { return ymd % 100; }

inline bool is_opex_week(int32_t ymd) {
    int dom = ymd % 100;
    return (dom >= 12 && dom <= 21);
}

// ============================================================================
// 1. Synchronized Multi-Asset 1-Minute MasterTimeline (UBSan Packed-Safe)
// ============================================================================
struct TimelineBar {
    int64_t ts_sec = 0;
    int32_t ymd = 0;
    int32_t tl_day_idx = 0;
    int16_t et_min = 0;
    int32_t bar_idx[3] = {-1, -1, -1};
    int32_t latest_bar[3] = {-1, -1, -1};
};

struct MasterTimeline {
    std::array<const PrecomputedMarketData*, 3> mds = {nullptr, nullptr, nullptr};
    std::vector<TimelineBar> bars;
    std::vector<int32_t> day_ymds;
    std::vector<size_t> day_tl_begin;
    std::vector<size_t> day_tl_end;
    std::vector<std::array<int32_t, 3>> day_last_sym_bar;
    std::array<std::vector<int32_t>, 3> b_to_tl;
};

inline MasterTimeline build_master_timeline(const std::array<const PrecomputedMarketData*, 3>& mds) {
    MasterTimeline tl;
    tl.mds = mds;
    size_t n_est = 0;
    for (int s = 0; s < 3; ++s) {
        if (mds[s]) {
            n_est = std::max(n_est, mds[s]->n_bars);
            tl.b_to_tl[s].assign(mds[s]->n_bars, -1);
        }
    }
    tl.bars.reserve(n_est + n_est / 8);

    std::array<size_t, 3> ptr = {0, 0, 0};
    std::array<int32_t, 3> cur_latest = {-1, -1, -1};
    int32_t cur_ymd = -1;
    std::array<int32_t, 3> cur_day_last = {-1, -1, -1};

    while (true) {
        int64_t min_ts = std::numeric_limits<int64_t>::max();
        for (int s = 0; s < 3; ++s) {
            if (mds[s] && ptr[s] < mds[s]->n_bars) {
                int64_t ts = mds[s]->bars[ptr[s]].ts_sec; // copy packed field before comparison
                if (ts < min_ts) min_ts = ts;
            }
        }
        if (min_ts == std::numeric_limits<int64_t>::max()) break;

        TimelineBar tb{};
        tb.ts_sec = min_ts;
        bool meta_set = false;
        for (int s = 0; s < 3; ++s) {
            if (mds[s] && ptr[s] < mds[s]->n_bars) {
                int64_t ts = mds[s]->bars[ptr[s]].ts_sec;
                if (ts == min_ts) {
                    int32_t bi = static_cast<int32_t>(ptr[s]);
                    tb.bar_idx[s] = bi;
                    cur_latest[s] = bi;
                    if (!meta_set) {
                        const auto& mm = mds[s]->meta[ptr[s]];
                        tb.ymd = mds[s]->day_yyyymmdd[static_cast<size_t>(mm.cme_day_idx)];
                        tb.et_min = mm.et_min;
                        meta_set = true;
                    }
                    ptr[s]++;
                }
            }
            tb.latest_bar[s] = cur_latest[s];
        }
        if (tb.ymd > CANONICAL_IS_END_YYYYMMDD) continue;

        if (tb.ymd != cur_ymd) {
            if (cur_ymd != -1) {
                tl.day_tl_end.push_back(tl.bars.size());
                tl.day_last_sym_bar.push_back(cur_day_last);
            }
            cur_ymd = tb.ymd;
            tl.day_ymds.push_back(cur_ymd);
            tl.day_tl_begin.push_back(tl.bars.size());
            cur_day_last = {-1, -1, -1};
        }
        for (int s = 0; s < 3; ++s) {
            if (tb.bar_idx[s] >= 0) cur_day_last[s] = tb.bar_idx[s];
        }
        tb.tl_day_idx = static_cast<int32_t>(tl.day_ymds.size() - 1);
        int32_t tl_i = static_cast<int32_t>(tl.bars.size());
        for (int s = 0; s < 3; ++s) {
            if (tb.bar_idx[s] >= 0) tl.b_to_tl[s][static_cast<size_t>(tb.bar_idx[s])] = tl_i;
        }
        tl.bars.push_back(tb);
    }
    if (cur_ymd != -1) {
        tl.day_tl_end.push_back(tl.bars.size());
        tl.day_last_sym_bar.push_back(cur_day_last);
    }
    return tl;
}

// ============================================================================
// 2. Causal Signal Event & Unified Zero-Bug Execution Primitives
// ============================================================================
struct CausalEntryEvent {
    int32_t ymd = 0;
    int32_t year = 2016;
    size_t entry_tl = 0;
    int8_t sess_id = 0;
    int8_t sym_id = 0;
    int8_t dir = 1;
    int8_t order_type = 0;       // 0=MktOpen, 1=RestingStop, 2=PullbackTouch
    int leg_id = 0;
    int fam_id = 0;
    bool is_tier_s = true;
    bool two_leg_split = false;
    bool is_mr_trail = false;
    bool is_clk2_eod950 = false;
    bool is_stack_htf60 = false;
    int def_m_safe = 1;

    double trigger_px = 0.0;
    double sl_pts = 10.0;
    double tp_pts = 20.0;
    double tp1_pts = 0.0;
    double ratchet_trig_pts = 0.0;
    double ratchet_trail_pts = 0.0;
    double be_trig_pts = 0.0;
    double be_offset_pts = 0.0;
    int max_hold_bars = 45;
    int stag_bars = 0;
    double stag_mfe_r = 0.35;
    int cooldown_bars = 1;
    double mr_tgt_px = 0.0;
    double mr_atr5_entry = 0.0;
    double kelly_mult = 1.0;
};

struct ActiveLegPos {
    int32_t group_id = 0;
    int leg_id = 0;
    int fam_id = 0;
    int8_t sym_id = 0;
    int8_t sess_id = 0;
    int8_t side = 1;
    int8_t sub_leg = 0;
    int micros = 1;
    size_t entry_tl = 0;
    int32_t entry_sym_b = 0;
    int32_t end_sym_b = 0;
    int32_t cooldown_bars = 1;

    double entry_px = 0.0;
    double stop_px = 0.0;
    double target_px = 0.0;
    double sl_pts = 10.0;
    double init_risk_usd = 0.0;
    double peak_mfe = 0.0;
    double peak_px = 0.0;
    double worst_mae_pts = 0.0;

    double r_trig = 0.0;
    double r_trail = 0.0;
    double be_trig = 0.0;
    double be_offset = 0.0;

    int stag_bars = 0;
    double stag_mfe_r = 0.35;
    bool two_leg_split = false;
    bool sub1_tp1_hit = false;
    bool tp1_applied_close = false;
    bool is_mr_trail = false;
    bool mr_trailing = false;
    double mr_tgt_px = 0.0;
    double mr_atr5_entry = 0.0;
    bool is_clk2_eod950 = false;
    double clk2_trail_r = 0.80;
    bool is_stack_htf60 = false;
};

struct ClosedLegTrade {
    int32_t ymd = 0;
    int32_t tl_day_idx = 0;
    int8_t sess_id = 0;
    int8_t sym_id = 0;
    int8_t side = 0;
    int leg_id = 0;
    int fam_id = 0;
    int micros = 0;
    size_t entry_tl = 0;
    size_t exit_tl = 0;
    int32_t entry_sym_b = 0;
    int32_t exit_sym_b = 0;
    double pts_net = 0.0;
    double net_usd = 0.0;
    double worst_adv_usd = 0.0;
    double init_risk_usd = 0.0;
    double peak_mfe = 0.0;
    uint8_t exit_reason = 0;
};

struct HtfCausalLevels {
    std::vector<double> prev_low, prev_high;
    std::vector<int8_t> htf_trend;
};

inline bool check_causal_entry_fill(
    int8_t order_type, int8_t dir, double trigger_px,
    const RawBar& eb_bar, double entry_slip_pts, double& fill_px_out)
{
    double ref_px = static_cast<double>(eb_bar.o);
    if (order_type == 1) {
        if (dir == 1) {
            if (static_cast<double>(eb_bar.h) < trigger_px) return false;
            ref_px = std::max(ref_px, trigger_px);
        } else {
            if (static_cast<double>(eb_bar.l) > trigger_px) return false;
            ref_px = std::min(ref_px, trigger_px);
        }
    } else if (order_type == 2) {
        if (dir == 1) {
            if (static_cast<double>(eb_bar.l) > trigger_px) return false;
            ref_px = std::min(ref_px, trigger_px);
        } else {
            if (static_cast<double>(eb_bar.h) < trigger_px) return false;
            ref_px = std::max(ref_px, trigger_px);
        }
    }
    fill_px_out = ref_px + static_cast<double>(dir) * entry_slip_pts;
    return true;
}

inline bool open_event_into_positions(
    const CausalEntryEvent& ev, const RawBar& eb_bar, int32_t eb_sym_b, int micros_tot,
    const InstrumentSpec& inst, int32_t group_id, std::vector<ActiveLegPos>& out_pos,
    double tp_override_pts = -1.0)
{
    out_pos.clear();
    if (micros_tot <= 0) return false;
    double fill_px = 0.0;
    if (!check_causal_entry_fill(ev.order_type, ev.dir, ev.trigger_px, eb_bar, inst.entry_slip_pts, fill_px)) {
        return false;
    }

    double sl_pts = std::max(ev.sl_pts, inst.tick_size * 4.0);
    double init_stop = quantize_stop_tick(fill_px - ev.dir * sl_pts, ev.dir, inst.tick_size);

    if (ev.is_mr_trail) {
        if ((ev.dir == 1 && ev.mr_tgt_px <= fill_px + inst.tick_size * 4.0) ||
            (ev.dir == -1 && ev.mr_tgt_px >= fill_px - inst.tick_size * 4.0)) {
            return false;
        }
        if (!validate_structural_side(ev.dir, init_stop, ev.mr_tgt_px)) return false;
    }

    auto make_sub = [&](int8_t sub_idx, int m, double tp_dist) -> bool {
        if (m <= 0) return true;
        double init_tp = (tp_dist > 0.0) ? (fill_px + ev.dir * tp_dist) : 0.0;
        if (init_tp > 0.0 && !validate_structural_side(ev.dir, init_stop, init_tp)) return false;
        ActiveLegPos ap{};
        ap.group_id = group_id;
        ap.leg_id = ev.leg_id;
        ap.fam_id = ev.fam_id;
        ap.sym_id = ev.sym_id;
        ap.sess_id = ev.sess_id;
        ap.side = ev.dir;
        ap.sub_leg = sub_idx;
        ap.micros = m;
        ap.entry_tl = ev.entry_tl;
        ap.entry_sym_b = eb_sym_b;
        ap.end_sym_b = eb_sym_b + std::max(1, ev.max_hold_bars);
        ap.cooldown_bars = ev.cooldown_bars;
        ap.entry_px = fill_px;
        ap.stop_px = init_stop;
        ap.target_px = init_tp;
        ap.sl_pts = sl_pts;
        double fee = resolve_rt_fee_usd(m, false, inst.fee_rt_micro, inst.fee_rt_mini);
        ap.init_risk_usd = (sl_pts + inst.entry_slip_pts + inst.exit_slip_pts) * inst.micro_pv * m + fee;
        ap.peak_mfe = 0.0;
        ap.peak_px = fill_px;
        ap.worst_mae_pts = 0.0;
        ap.r_trig = ev.ratchet_trig_pts;
        ap.r_trail = ev.ratchet_trail_pts;
        ap.be_trig = ev.be_trig_pts;
        ap.be_offset = ev.be_offset_pts;
        ap.stag_bars = ev.stag_bars;
        ap.stag_mfe_r = ev.stag_mfe_r;
        ap.two_leg_split = ev.two_leg_split;
        ap.is_mr_trail = ev.is_mr_trail;
        ap.mr_tgt_px = ev.mr_tgt_px;
        ap.mr_atr5_entry = ev.mr_atr5_entry;
        ap.is_clk2_eod950 = ev.is_clk2_eod950;
        ap.is_stack_htf60 = ev.is_stack_htf60;
        out_pos.push_back(ap);
        return true;
    };

    if (ev.two_leg_split) {
        int m1 = (micros_tot == 1) ? 1 : std::max(1, static_cast<int>(std::lround(micros_tot * 0.65)));
        int m2 = micros_tot - m1;
        if (!make_sub(0, m1, ev.tp1_pts)) { out_pos.clear(); return false; }
        if (!make_sub(1, m2, ev.tp_pts))  { out_pos.clear(); return false; }
    } else {
        double eff_tp = ev.tp_pts;
        if (tp_override_pts > 0.0) {
            eff_tp = (eff_tp > 0.0) ? std::min(eff_tp, tp_override_pts) : tp_override_pts;
        }
        if (!make_sub(0, micros_tot, eff_tp)) { out_pos.clear(); return false; }
    }
    return !out_pos.empty();
}

struct SingleLegBarSim {
    double raw_pts_net = 0.0;
    double worst_adv_pts = 0.0;
    double peak_mfe_pts = 0.0;
    size_t exit_b = 0;
    int32_t exit_ymd = 20160101;
    uint8_t exit_reason = 0;
};

inline SingleLegBarSim simulate_single_bar_seq_bugfree(
    const PrecomputedMarketData& md, size_t eb, int8_t dir, double ep,
    double sl_pts, double tp_pts, int max_hold_bars,
    double r_trig, double r_trail, double be_trig, double be_offset,
    int stag_bars, double stag_mfe_r,
    bool is_clk2_eod950, bool is_mr_trail, double mr_tgt_px, double mr_atr5_entry,
    const std::vector<bool>* is_5m_close, const InstrumentSpec& inst)
{
    SingleLegBarSim res{};
    const double tick = inst.tick_size, slip_out = inst.exit_slip_pts;
    const int32_t cday = md.meta[eb].cme_day_idx;
    const size_t end_b = std::min(md.n_bars - 1, eb + static_cast<size_t>(std::max(1, max_hold_bars)));
    double stop_px = quantize_stop_tick(ep - dir * sl_pts, dir, tick);
    double target_px = (tp_pts > 0.0) ? (ep + dir * tp_pts) : 0.0;
    double peak_mfe = 0.0, peak_px = ep, wadv = 0.0, pnl_pts = 0.0;
    bool mr_trailing = false, closed = false;
    size_t exit_b = end_b;
    uint8_t reason = 2;

    for (size_t k = eb; k <= end_b; ++k) {
        if (k > eb && md.meta[k].cme_day_idx != cday) {
            pnl_pts = dir * (static_cast<double>(md.bars[k - 1].c) - ep) - slip_out;
            wadv = std::max(wadv, -pnl_pts);
            exit_b = k - 1;
            reason = 3;
            closed = true;
            break;
        }
        const double bo = md.bars[k].o, bh = md.bars[k].h, bl = md.bars[k].l, bc = md.bars[k].c;
        if (is_clk2_eod950 && md.meta[k].et_min >= 950) {
            pnl_pts = dir * (bc - ep) - slip_out;
            wadv = std::max(wadv, -pnl_pts);
            exit_b = k;
            reason = 3;
            closed = true;
            break;
        }
        double adv = (dir == 1) ? (ep - bl) : (bh - ep);
        double fav = (dir == 1) ? (bh - ep) : (ep - bl);
        wadv = std::max(wadv, adv + slip_out);

        bool hit_sl = (dir == 1) ? (bl <= stop_px) : (bh >= stop_px);
        if (hit_sl) {
            double ref = (dir == 1) ? ((bo < stop_px) ? bo : stop_px) : ((bo > stop_px) ? bo : stop_px);
            double exit_px = (dir == 1) ? (ref - slip_out) : (ref + slip_out);
            pnl_pts = dir * (exit_px - ep);
            wadv = std::max(wadv, -pnl_pts);
            exit_b = k;
            reason = 0;
            closed = true;
            break;
        }

        if (target_px > 0.0) {
            bool hit_tp = (dir == 1) ? (bh >= target_px) : (bl <= target_px);
            if (hit_tp) {
                if (fav > peak_mfe) peak_mfe = fav;
                double exit_px = (dir == 1) ? (target_px - slip_out) : (target_px + slip_out);
                pnl_pts = dir * (exit_px - ep);
                wadv = std::max(wadv, -pnl_pts);
                exit_b = k;
                reason = 1;
                closed = true;
                break;
            }
        }

        if (k == end_b) {
            pnl_pts = dir * (bc - ep) - slip_out;
            wadv = std::max(wadv, -pnl_pts);
            exit_b = k;
            reason = 2;
            closed = true;
            break;
        }
        if (fav > peak_mfe) peak_mfe = fav;
        peak_px = (dir == 1) ? std::max(peak_px, bh) : std::min(peak_px, bl);

        if (stag_bars > 0 && static_cast<int>(k - eb) >= stag_bars && peak_mfe < stag_mfe_r * sl_pts) {
            double cf = dir * (bc - ep);
            if (cf < 0.0) {
                pnl_pts = cf - slip_out;
                wadv = std::max(wadv, -pnl_pts);
                exit_b = k;
                reason = 2;
                closed = true;
                break;
            }
        }

        double new_stop = stop_px;
        bool stop_moved = false;
        if (be_trig > 0.0 && peak_mfe >= be_trig) {
            double q = quantize_stop_tick(ep + dir * be_offset, dir, tick);
            if ((dir == 1 && q > new_stop) || (dir == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
        }
        if (r_trig > 0.0 && peak_mfe >= r_trig) {
            double lock = std::max(r_trig - r_trail, peak_mfe - r_trail);
            double q = quantize_stop_tick(ep + dir * lock, dir, tick);
            if ((dir == 1 && q > new_stop) || (dir == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
        }
        if (is_mr_trail && is_5m_close && (*is_5m_close)[k]) {
            if (mr_trailing) {
                double raw_s = (dir == 1) ? (peak_px - 0.8 * mr_atr5_entry) : (peak_px + 0.8 * mr_atr5_entry);
                double q = quantize_stop_tick(raw_s, dir, tick);
                if ((dir == 1 && q > new_stop) || (dir == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
            } else {
                if ((dir == 1 && bc >= mr_tgt_px) || (dir == -1 && bc <= mr_tgt_px)) {
                    mr_trailing = true;
                    double q = quantize_stop_tick(mr_tgt_px, dir, tick);
                    if ((dir == 1 && q > new_stop) || (dir == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                }
            }
        }
        if (stop_moved) {
            if ((dir == 1 && bc <= new_stop) || (dir == -1 && bc >= new_stop)) {
                pnl_pts = dir * (bc - ep) - slip_out;
                wadv = std::max(wadv, -pnl_pts);
                exit_b = k;
                reason = 0;
                closed = true;
                break;
            }
            stop_px = (dir == 1) ? std::min(new_stop, bc - tick) : std::max(new_stop, bc + tick);
        }
    }
    if (!closed) {
        pnl_pts = dir * (static_cast<double>(md.bars[end_b].c) - ep) - slip_out;
        wadv = std::max(wadv, -pnl_pts);
        exit_b = end_b;
    }
    res.raw_pts_net = pnl_pts;
    res.worst_adv_pts = std::max(wadv, -pnl_pts);
    res.peak_mfe_pts = peak_mfe;
    res.exit_b = exit_b;
    res.exit_ymd = md.day_yyyymmdd[static_cast<size_t>(md.meta[exit_b].cme_day_idx)];
    res.exit_reason = reason;
    return res;
}

// ============================================================================
// 3. Generic Regularized Expanding Histogram Depth-2 GBDT
// ============================================================================
inline double sigmoid_safe(double z) {
    if (z > 30.0) return 1.0;
    if (z < -30.0) return 0.0;
    return 1.0 / (1.0 + std::exp(-z));
}

template <size_t NF>
struct HistTree2Gen {
    size_t r_f = 0, l_f = 0, rr_f = 0;
    uint8_t r_b = 7, l_b = 7, rr_b = 7;
    double v00 = 0.0, v01 = 0.0, v10 = 0.0, v11 = 0.0;

    double eval(const std::array<double, NF>& mu, const std::array<double, NF>& sd,
                const std::array<float, NF>& x) const
    {
        static constexpr double cuts[15] = {-2.2, -1.75, -1.40, -1.10, -0.85, -0.60, -0.40, -0.20, 0.0, 0.20, 0.40, 0.60, 0.85, 1.10, 1.45};
        auto get_bin = [&](size_t f) -> uint8_t {
            double z = (static_cast<double>(x[f]) - mu[f]) / sd[f];
            uint8_t b = 0;
            for (int k = 0; k < 15; ++k) if (z > cuts[k]) b++;
            return b;
        };
        if (get_bin(r_f) <= r_b) return (get_bin(l_f) <= l_b) ? v00 : v01;
        return (get_bin(rr_f) <= rr_b) ? v10 : v11;
    }
};

template <size_t NF>
struct HistGBDT2Gen {
    std::array<double, NF> mu{}, sd{};
    double base_score = 0.0;
    std::vector<HistTree2Gen<NF>> trees;
    bool valid = false;

    double predict(const std::array<float, NF>& x) const {
        if (!valid) return 0.5;
        double s = base_score;
        for (const auto& t : trees) s += t.eval(mu, sd, x);
        return sigmoid_safe(s);
    }
};

template <size_t NF>
struct TrainSampleGen {
    double target = 0.0, weight = 1.0;
    std::array<float, NF> x{};
};

template <size_t NF>
inline HistGBDT2Gen<NF> train_hist_gbdt2_gen(
    const std::vector<const TrainSampleGen<NF>*>& tr, bool use_weights,
    int n_trees = 50, double lambda = 80.0, size_t min_leaf_rows = 25,
    double lr = 0.06, size_t min_rows = 60)
{
    static constexpr double cuts[15] = {-2.2, -1.75, -1.40, -1.10, -0.85, -0.60, -0.40, -0.20, 0.0, 0.20, 0.40, 0.60, 0.85, 1.10, 1.45};
    HistGBDT2Gen<NF> m;
    m.sd.fill(1.0);
    const size_t n = tr.size();
    if (n < min_rows) return m;
    const double dn = static_cast<double>(n);
    for (size_t j = 0; j < NF; ++j) {
        double s = 0.0;
        for (auto* p : tr) s += static_cast<double>(p->x[j]);
        m.mu[j] = s / dn;
        double ss = 0.0;
        for (auto* p : tr) { double d = static_cast<double>(p->x[j]) - m.mu[j]; ss += d * d; }
        m.sd[j] = std::max(1e-6, std::sqrt(ss / dn));
    }

    std::vector<std::array<uint8_t, NF>> bins(n);
    std::vector<double> Y(n), W(n), f(n, 0.0);
    double wpos = 0.0, wneg = 0.0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < NF; ++j) {
            double z = (static_cast<double>(tr[i]->x[j]) - m.mu[j]) / m.sd[j];
            uint8_t b = 0;
            for (int k = 0; k < 15; ++k) if (z > cuts[k]) b++;
            bins[i][j] = b;
        }
        Y[i] = (tr[i]->target > 0.0) ? 1.0 : 0.0;
        W[i] = use_weights ? std::clamp(tr[i]->weight, 0.25, 3.0) : 1.0;
        if (Y[i] > 0.5) wpos += W[i]; else wneg += W[i];
    }
    m.base_score = std::log(std::max(1e-6, wpos) / std::max(1e-6, wneg));
    std::fill(f.begin(), f.end(), m.base_score);
    m.valid = true;

    const size_t eff_min_leaf = std::max<size_t>(18, std::min(min_leaf_rows, n / 8));
    std::vector<double> g(n), h(n);
    std::vector<size_t> all_idx(n), left_idx, right_idx;
    std::iota(all_idx.begin(), all_idx.end(), 0);
    left_idx.reserve(n); right_idx.reserve(n);

    auto find_best_split = [&](const std::vector<size_t>& idxs, size_t& best_f, uint8_t& best_b,
                               double& best_gain, double& vL, double& vR) {
        double Ga = 0.0, Ha = 0.0;
        size_t Ca = idxs.size();
        double Gh[NF][16] = {}, Hh[NF][16] = {};
        size_t Ch[NF][16] = {};
        for (size_t idx : idxs) {
            double gi = g[idx], hi = h[idx];
            Ga += gi; Ha += hi;
            const auto& row = bins[idx];
            for (size_t j = 0; j < NF; ++j) {
                uint8_t b = row[j];
                Gh[j][b] += gi; Hh[j][b] += hi; Ch[j][b]++;
            }
        }
        double base_obj = (Ga * Ga) / (Ha + lambda);
        double single_leaf = lr * (Ga / (Ha + lambda));
        vL = single_leaf; vR = single_leaf; best_gain = 0.0; best_f = 0; best_b = 7;
        for (size_t j = 0; j < NF; ++j) {
            double gL = 0.0, hL = 0.0;
            size_t cL = 0;
            for (uint8_t b = 0; b < 15; ++b) {
                gL += Gh[j][b]; hL += Hh[j][b]; cL += Ch[j][b];
                size_t cR = Ca - cL;
                double gR = Ga - gL, hR = Ha - hL;
                if (cL < eff_min_leaf || cR < eff_min_leaf || hL < 4.0 || hR < 4.0) continue;
                double gain = (gL * gL) / (hL + lambda) + (gR * gR) / (hR + lambda) - base_obj;
                if (gain > best_gain) {
                    best_gain = gain; best_f = j; best_b = b;
                    vL = lr * (gL / (hL + lambda)); vR = lr * (gR / (hR + lambda));
                }
            }
        }
    };

    for (int t = 0; t < n_trees; ++t) {
        for (size_t i = 0; i < n; ++i) {
            double p = sigmoid_safe(f[i]);
            g[i] = W[i] * (Y[i] - p); h[i] = W[i] * p * (1.0 - p) + 1e-6;
        }
        HistTree2Gen<NF> tree{};
        double rgain = 0.0, rvL = 0.0, rvR = 0.0;
        find_best_split(all_idx, tree.r_f, tree.r_b, rgain, rvL, rvR);
        if (rgain <= 1e-6) break;
        left_idx.clear(); right_idx.clear();
        for (size_t idx : all_idx) {
            if (bins[idx][tree.r_f] <= tree.r_b) left_idx.push_back(idx);
            else right_idx.push_back(idx);
        }
        double lg = 0.0, rg2 = 0.0;
        find_best_split(left_idx,  tree.l_f,  tree.l_b,  lg,  tree.v00, tree.v01);
        find_best_split(right_idx, tree.rr_f, tree.rr_b, rg2, tree.v10, tree.v11);
        m.trees.push_back(tree);
        for (size_t idx : left_idx) f[idx] += (bins[idx][tree.l_f] <= tree.l_b) ? tree.v00 : tree.v01;
        for (size_t idx : right_idx) f[idx] += (bins[idx][tree.rr_f] <= tree.rr_b) ? tree.v10 : tree.v11;
    }
    return m;
}

// ============================================================================
// 4. Simultaneous Multi-Asset Simulator & Upgraded Consistency-Locked Combine
// ============================================================================
struct CombinePolicyV7 {
    std::string label;
    int mode = 0; // 0 = CPPI, 1 = Shield-B, 2 = Flat
    double m_buf = 1350.0;
    double m_mult = 0.38;
    double r_min = 40.0;
    double r_max = 1000.0;
    double up_thr = 750.0;
    double up_risk = 1000.0;
    double daily_stop = 750.0;
    double ds_floor = 150.0;
    double ds_alpha = 0.65;
    double open_heat_frac = 0.75;
    double dcap = 1500.0;
    bool target_cap = true;
    bool tier_dd_gate = false;
    bool prof_lock = false;
    bool confluence_boost = false;
    bool streak_brake = false;
    // Wave 7 Upgrades (Strictly Causal at Entry Open):
    int max_open_total = 2;
    int max_open_per_sym = 1;
    bool cons_tp_clamp = true;
    double dd_shield_limit = 0.0;
};

struct SimPortfolioResult {
    std::vector<ClosedLegTrade> trades;
    std::vector<TradeRecord> u11_trades;
    std::vector<int8_t> u11_trade_syms;
    std::vector<DayRecord> days;
    double net_usd = 0.0;
    double curve_max_dd = 0.0;
    double bar_max_dd = 0.0;
    double h1_net = 0.0;
    double h2_net = 0.0;
    double mod_net = 0.0;
    int single_dll_days = 0;
    int consec_dll_days = 0;
};

inline SimPortfolioResult run_simultaneous_portfolio_sim(
    const MasterTimeline& tl,
    const std::array<const HtfCausalLevels*, 3>& htf_ptrs,
    const std::array<const std::vector<bool>*, 3>& mr_5m_ptrs,
    const std::vector<CausalEntryEvent>& events_sorted,
    const std::array<InstrumentSpec, 3>& insts,
    int32_t min_ymd, int32_t max_ymd,
    double risk_usd, bool use_def_msafe_on_stack,
    bool enforce_prop_limits, double daily_dll_usd = 1000.0, double daily_profit_cap_usd = 1500.0,
    int max_open_total = 64, int max_open_per_sym = 64)
{
    SimPortfolioResult res;
    if (tl.day_ymds.empty()) return res;

    size_t d_start = 0, d_end = tl.day_ymds.size();
    while (d_start < d_end && tl.day_ymds[d_start] < min_ymd) ++d_start;
    while (d_end > d_start && tl.day_ymds[d_end - 1] > max_ymd) --d_end;
    if (d_start >= d_end) return res;

    std::vector<std::vector<const CausalEntryEvent*>> day_events(d_end - d_start);
    for (const auto& ev : events_sorted) {
        if (ev.ymd < min_ymd || ev.ymd > max_ymd) continue;
        int32_t tdi = tl.bars[ev.entry_tl].tl_day_idx;
        if (tdi >= static_cast<int32_t>(d_start) && tdi < static_cast<int32_t>(d_end)) {
            day_events[static_cast<size_t>(tdi) - d_start].push_back(&ev);
        }
    }

    std::vector<ActiveLegPos> pool;
    pool.reserve(64);
    std::vector<ActiveLegPos> spawned;
    spawned.reserve(4);
    std::vector<uint8_t> group_tp1_hit;

    std::array<bool, MAX_LEGS> leg_open{};
    std::array<int32_t, MAX_LEGS> leg_cool_end{};
    leg_cool_end.fill(-1);

    double cum_eq = 0.0, bar_peak_eq = 0.0, curve_peak_eq = 0.0;
    bool prev_dll = false;
    res.days.reserve(d_end - d_start);

    for (size_t d = d_start; d < d_end; ++d) {
        const auto& evs = day_events[d - d_start];
        size_t t = tl.day_tl_begin[d];
        size_t t_end = tl.day_tl_end[d];
        size_t ev_ptr = 0;

        pool.clear();
        group_tp1_hit.clear();
        leg_open.fill(false);
        leg_cool_end.fill(-1);

        double day_pnl = 0.0, day_worst_intra = 0.0;
        double sess_pnl[4] = {};
        int day_trades = 0;
        bool day_halted = false, hit_dll_today = false;
        int8_t dll_sess = -1;

        auto realize_closed = [&](const ActiveLegPos& pos, double exit_px, size_t x_tl, int32_t x_sb,
                                  uint8_t reason) {
            const InstrumentSpec& inst = insts[static_cast<size_t>(pos.sym_id)];
            double pts = (pos.side == 1) ? (exit_px - pos.entry_px) : (pos.entry_px - exit_px);
            double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
            double usd = pts * inst.micro_pv * pos.micros - fee;
            double tracked_wadv = -pos.worst_mae_pts * inst.micro_pv * pos.micros - fee;
            double wadv_usd = std::min(usd, tracked_wadv);
            day_pnl += usd;
            cum_eq += usd;
            if (cum_eq > curve_peak_eq) curve_peak_eq = cum_eq;
            res.curve_max_dd = std::max(res.curve_max_dd, curve_peak_eq - cum_eq);
            if (pos.sess_id >= 0 && pos.sess_id < 4) sess_pnl[pos.sess_id] += usd;
            day_trades++;

            int32_t ymd = tl.day_ymds[d];
            if (ymd < 20220101) res.h1_net += usd;
            else res.h2_net += usd;
            if (ymd >= 20230101) res.mod_net += usd;

            ClosedLegTrade ct{};
            ct.ymd = ymd; ct.tl_day_idx = static_cast<int32_t>(d);
            ct.sess_id = pos.sess_id; ct.sym_id = pos.sym_id; ct.side = pos.side;
            ct.leg_id = pos.leg_id; ct.fam_id = pos.fam_id; ct.micros = pos.micros;
            ct.entry_tl = pos.entry_tl; ct.exit_tl = x_tl;
            ct.entry_sym_b = pos.entry_sym_b; ct.exit_sym_b = x_sb;
            ct.pts_net = pts; ct.net_usd = usd; ct.worst_adv_usd = wadv_usd;
            ct.init_risk_usd = pos.init_risk_usd;
            ct.peak_mfe = std::max(0.0, std::max(pos.peak_mfe, pts));
            ct.exit_reason = reason;
            res.trades.push_back(ct);

            TradeRecord ur{};
            ur.entry_bar = static_cast<size_t>(pos.entry_sym_b);
            ur.exit_bar = static_cast<size_t>(x_sb);
            ur.cme_day_idx = static_cast<int32_t>(d - d_start);
            ur.cme_yyyymmdd = ymd;
            ur.sess_id = pos.sess_id;
            ur.side = pos.side;
            ur.contracts_micros = pos.micros;
            ur.entry_px = pos.entry_px;
            ur.exit_px = exit_px;
            ur.pts_net = pts;
            ur.net_usd = usd;
            ur.initial_risk_usd = pos.init_risk_usd;
            ur.r_multiple = (pos.init_risk_usd > 0.0) ? (usd / pos.init_risk_usd) : 0.0;
            ur.peak_mfe_pts = ct.peak_mfe;
            ur.exit_reason = reason;
            res.u11_trades.push_back(ur);
            res.u11_trade_syms.push_back(pos.sym_id);

            bool still_open = false;
            for (const auto& op : pool) {
                if (&op != &pos && op.group_id == pos.group_id) { still_open = true; break; }
            }
            if (!still_open && pos.leg_id >= 0 && static_cast<size_t>(pos.leg_id) < MAX_LEGS) {
                leg_open[static_cast<size_t>(pos.leg_id)] = false;
                leg_cool_end[static_cast<size_t>(pos.leg_id)] = x_sb + pos.cooldown_bars;
            }
            return usd;
        };

        while (t < t_end && !day_halted) {
            if (pool.empty()) {
                if (ev_ptr >= evs.size()) break;
                t = std::max(t, evs[ev_ptr]->entry_tl);
                if (t >= t_end) break;
            }
            const auto& tb = tl.bars[t];
            while (ev_ptr < evs.size() && evs[ev_ptr]->entry_tl < t) ++ev_ptr;

            while (ev_ptr < evs.size() && evs[ev_ptr]->entry_tl == t) {
                const CausalEntryEvent* ev = evs[ev_ptr++];
                size_t lid = static_cast<size_t>(std::clamp(ev->leg_id, 0, static_cast<int>(MAX_LEGS - 1)));
                int32_t sb = tb.bar_idx[static_cast<size_t>(ev->sym_id)];
                if (sb < 0 || leg_open[lid] || (leg_cool_end[lid] >= 0 && sb <= leg_cool_end[lid])) continue;

                int open_total = 0, open_sym = 0;
                int act_m = 0;
                for (size_t pi = 0; pi < pool.size(); ++pi) {
                    act_m += pool[pi].micros;
                    bool dup = false;
                    for (size_t pj = 0; pj < pi; ++pj) {
                        if (pool[pj].group_id == pool[pi].group_id) { dup = true; break; }
                    }
                    if (!dup) {
                        open_total++;
                        if (pool[pi].sym_id == ev->sym_id) open_sym++;
                    }
                }
                if (open_total >= max_open_total || open_sym >= max_open_per_sym) continue;
                int room = 50 - act_m;
                if (room <= 0) continue;

                const InstrumentSpec& inst = insts[static_cast<size_t>(ev->sym_id)];
                int want = (use_def_msafe_on_stack && ev->two_leg_split)
                    ? ev->def_m_safe
                    : std::clamp(static_cast<int>(std::lround(risk_usd * ev->kelly_mult / (ev->sl_pts * inst.micro_pv))), 1, 50);
                int m = std::min(want, room);
                if (m <= 0) continue;

                int32_t gid = static_cast<int32_t>(group_tp1_hit.size());
                if (open_event_into_positions(*ev, tl.mds[ev->sym_id]->bars[sb], sb, m, inst, gid, spawned)) {
                    group_tp1_hit.push_back(0);
                    leg_open[lid] = true;
                    for (const auto& sp : spawned) pool.push_back(sp);
                }
            }

            if (pool.empty()) { ++t; continue; }

            double min_real_day = day_pnl;
            double max_real_day = day_pnl;

            // 3A: Stops first
            for (size_t i = 0; i < pool.size();) {
                auto& pos = pool[i];
                int32_t sb = tb.bar_idx[static_cast<size_t>(pos.sym_id)];
                if (sb < 0) { ++i; continue; }
                const auto& bar = tl.mds[pos.sym_id]->bars[sb];
                const InstrumentSpec& inst = insts[static_cast<size_t>(pos.sym_id)];
                const double bo = bar.o, bh = bar.h, bl = bar.l;
                if (pos.is_clk2_eod950 && tl.mds[pos.sym_id]->meta[sb].et_min >= 950) {
                    double ex = (pos.side == 1) ? (bar.c - inst.exit_slip_pts) : (bar.c + inst.exit_slip_pts);
                    realize_closed(pos, ex, t, sb, 3);
                    min_real_day = std::min(min_real_day, day_pnl);
                    max_real_day = std::max(max_real_day, day_pnl);
                    pool[i] = pool.back(); pool.pop_back();
                    continue;
                }
                bool hit_sl = (pos.side == 1) ? (bl <= pos.stop_px) : (bh >= pos.stop_px);
                if (hit_sl) {
                    double ref = (pos.side == 1) ? ((bo < pos.stop_px) ? bo : pos.stop_px)
                                                 : ((bo > pos.stop_px) ? bo : pos.stop_px);
                    double ex = (pos.side == 1) ? (ref - inst.exit_slip_pts) : (ref + inst.exit_slip_pts);
                    realize_closed(pos, ex, t, sb, 0);
                    min_real_day = std::min(min_real_day, day_pnl);
                    max_real_day = std::max(max_real_day, day_pnl);
                    pool[i] = pool.back(); pool.pop_back();
                    continue;
                }
                ++i;
            }

            // 3B: Simultaneous MTM
            double worst_unr = 0.0, best_unr = 0.0;
            if (!pool.empty()) {
                double sym_l_unr[3] = {}, sym_h_unr[3] = {};
                bool sym_has[3] = {};
                for (auto& pos : pool) {
                    int s = pos.sym_id;
                    int32_t lb = (tb.bar_idx[s] >= 0) ? tb.bar_idx[s] : tb.latest_bar[s];
                    if (lb < 0) continue;
                    sym_has[s] = true;
                    const auto& bar = tl.mds[s]->bars[lb];
                    const InstrumentSpec& inst = insts[s];
                    double adv_px = (pos.side == 1) ? (bar.l - inst.exit_slip_pts) : (bar.h + inst.exit_slip_pts);
                    double adv_pts = (pos.side == 1) ? (pos.entry_px - adv_px) : (adv_px - pos.entry_px);
                    if (adv_pts > pos.worst_mae_pts) pos.worst_mae_pts = adv_pts;
                    double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                    double pv = inst.micro_pv;
                    auto eval_px = [&](double px) {
                        double ex = (pos.side == 1) ? (px - inst.exit_slip_pts) : (px + inst.exit_slip_pts);
                        double pts = (pos.side == 1) ? (ex - pos.entry_px) : (pos.entry_px - ex);
                        return pts * pv * pos.micros - fee;
                    };
                    double eff_l = (pos.side == -1 && pos.target_px > 0.0) ? std::max<double>(bar.l, pos.target_px) : static_cast<double>(bar.l);
                    double eff_h = (pos.side == 1  && pos.target_px > 0.0) ? std::min<double>(bar.h, pos.target_px) : static_cast<double>(bar.h);
                    sym_l_unr[s] += eval_px(eff_l);
                    sym_h_unr[s] += eval_px(eff_h);
                }
                for (int s = 0; s < 3; ++s) {
                    if (!sym_has[s]) continue;
                    worst_unr += std::min(sym_l_unr[s], sym_h_unr[s]);
                    best_unr += std::max(sym_l_unr[s], sym_h_unr[s]);
                }
            }
            double day_start_eq = cum_eq - day_pnl;
            double worst_day_eq = min_real_day + worst_unr;
            double best_day_eq  = max_real_day + best_unr;
            day_worst_intra = std::min(day_worst_intra, worst_day_eq);

            double bar_best_total = day_start_eq + best_day_eq;
            if (bar_best_total > bar_peak_eq) bar_peak_eq = bar_best_total;
            double bar_worst_total = day_start_eq + worst_day_eq;
            res.bar_max_dd = std::max(res.bar_max_dd, bar_peak_eq - bar_worst_total);

            if (enforce_prop_limits && (worst_day_eq <= -daily_dll_usd || day_pnl <= -daily_dll_usd)) {
                if (!pool.empty()) {
                    double rem_needed = (-daily_dll_usd) - day_pnl;
                    size_t nr = pool.size();
                    for (size_t k = 0; k < nr; ++k) {
                        const auto& pos = pool[k];
                        const InstrumentSpec& inst = insts[pos.sym_id];
                        double tgt_u = rem_needed / static_cast<double>(nr);
                        double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                        double pts_n = (tgt_u + fee) / (inst.micro_pv * pos.micros);
                        double liq_px = (pos.side == 1) ? (pos.entry_px + pts_n) : (pos.entry_px - pts_n);
                        int32_t lb = (tb.bar_idx[pos.sym_id] >= 0) ? tb.bar_idx[pos.sym_id] : tb.latest_bar[pos.sym_id];
                        realize_closed(pos, liq_px, t, lb, 4);
                    }
                    pool.clear();
                }
                hit_dll_today = true;
                dll_sess = tb.bar_idx[0] >= 0 ? tl.mds[0]->meta[tb.bar_idx[0]].sess_id : 2;
                day_halted = true;
                break;
            }

            // 3C: TPs after SL/DLL
            for (size_t i = 0; i < pool.size();) {
                auto& pos = pool[i];
                int32_t sb = tb.bar_idx[static_cast<size_t>(pos.sym_id)];
                if (sb < 0 || pos.target_px <= 0.0) { ++i; continue; }
                const auto& bar = tl.mds[pos.sym_id]->bars[sb];
                const InstrumentSpec& inst = insts[static_cast<size_t>(pos.sym_id)];
                bool hit_tp = (pos.side == 1) ? (bar.h >= pos.target_px) : (bar.l <= pos.target_px);
                if (hit_tp) {
                    double fav = (pos.side == 1) ? (bar.h - pos.entry_px) : (pos.entry_px - bar.l);
                    pos.peak_mfe = std::max(pos.peak_mfe, fav);
                    double ex = (pos.side == 1) ? (pos.target_px - inst.exit_slip_pts) : (pos.target_px + inst.exit_slip_pts);
                    realize_closed(pos, ex, t, sb, 1);
                    if (pos.two_leg_split && pos.sub_leg == 0) {
                        group_tp1_hit[static_cast<size_t>(pos.group_id)] = 1;
                    }
                    pool[i] = pool.back(); pool.pop_back();
                    continue;
                }
                ++i;
            }

            if (enforce_prop_limits && day_pnl >= daily_profit_cap_usd) {
                for (size_t k = 0; k < pool.size(); ++k) {
                    const auto& pos = pool[k];
                    int32_t lb = (tb.bar_idx[pos.sym_id] >= 0) ? tb.bar_idx[pos.sym_id] : tb.latest_bar[pos.sym_id];
                    double mc = tl.mds[pos.sym_id]->bars[lb].c;
                    const InstrumentSpec& inst = insts[pos.sym_id];
                    double ex = (pos.side == 1) ? (mc - inst.exit_slip_pts) : (mc + inst.exit_slip_pts);
                    realize_closed(pos, ex, t, lb, 6);
                }
                pool.clear();
                day_halted = true;
                break;
            }

            // 3D: Time/Stag/Ratchets
            for (size_t i = 0; i < pool.size();) {
                auto& pos = pool[i];
                int32_t sb = tb.bar_idx[static_cast<size_t>(pos.sym_id)];
                if (sb < 0) { ++i; continue; }
                const auto* md_p = tl.mds[pos.sym_id];
                const auto& bar = md_p->bars[sb];
                const InstrumentSpec& inst = insts[pos.sym_id];
                const double bc = bar.c;

                if (sb >= pos.end_sym_b) {
                    double ex = (pos.side == 1) ? (bc - inst.exit_slip_pts) : (bc + inst.exit_slip_pts);
                    realize_closed(pos, ex, t, sb, 2);
                    pool[i] = pool.back(); pool.pop_back();
                    continue;
                }

                double fav = (pos.side == 1) ? (bar.h - pos.entry_px) : (pos.entry_px - bar.l);
                if (fav > pos.peak_mfe) pos.peak_mfe = fav;
                pos.peak_px = (pos.side == 1) ? std::max<double>(pos.peak_px, bar.h) : std::min<double>(pos.peak_px, bar.l);

                if (pos.stag_bars > 0 && (sb - pos.entry_sym_b) >= pos.stag_bars && pos.peak_mfe < pos.stag_mfe_r * pos.sl_pts) {
                    double cf = pos.side * (bc - pos.entry_px);
                    if (cf < 0.0) {
                        double ex = (pos.side == 1) ? (bc - inst.exit_slip_pts) : (bc + inst.exit_slip_pts);
                        realize_closed(pos, ex, t, sb, 2);
                        pool[i] = pool.back(); pool.pop_back();
                        continue;
                    }
                }

                double new_stop = pos.stop_px;
                bool stop_moved = false;

                if (pos.be_trig > 0.0 && pos.peak_mfe >= pos.be_trig) {
                    double q = quantize_stop_tick(pos.entry_px + pos.side * pos.be_offset, pos.side, inst.tick_size);
                    if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                }
                if (pos.r_trig > 0.0 && pos.peak_mfe >= pos.r_trig) {
                    double lock = std::max(pos.r_trig - pos.r_trail, pos.peak_mfe - pos.r_trail);
                    double q = quantize_stop_tick(pos.entry_px + pos.side * lock, pos.side, inst.tick_size);
                    if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                }
                if (pos.is_mr_trail && mr_5m_ptrs[pos.sym_id] && (*mr_5m_ptrs[pos.sym_id])[sb]) {
                    if (pos.mr_trailing) {
                        double raw_s = (pos.side == 1) ? (pos.peak_px - 0.8 * pos.mr_atr5_entry) : (pos.peak_px + 0.8 * pos.mr_atr5_entry);
                        double q = quantize_stop_tick(raw_s, pos.side, inst.tick_size);
                        if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                    } else {
                        if ((pos.side == 1 && bc >= pos.mr_tgt_px) || (pos.side == -1 && bc <= pos.mr_tgt_px)) {
                            pos.mr_trailing = true;
                            double q = quantize_stop_tick(pos.mr_tgt_px, pos.side, inst.tick_size);
                            if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                        }
                    }
                }
                if (pos.two_leg_split && pos.sub_leg == 1 && group_tp1_hit[pos.group_id]) {
                    if (!pos.tp1_applied_close) {
                        pos.tp1_applied_close = true;
                        double q = quantize_stop_tick(pos.entry_px + pos.side * inst.tick_size, pos.side, inst.tick_size);
                        if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                    }
                    if (pos.is_stack_htf60 && htf_ptrs[pos.sym_id] && (sb % 60 == 0)) {
                        const auto* hp = htf_ptrs[pos.sym_id];
                        double hpx = (pos.side == 1) ? (hp->prev_low[sb] - inst.tick_size) : (hp->prev_high[sb] + inst.tick_size);
                        double q = quantize_stop_tick(hpx, pos.side, inst.tick_size);
                        if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                    }
                }

                if (stop_moved) {
                    if ((pos.side == 1 && bc <= new_stop) || (pos.side == -1 && bc >= new_stop)) {
                        double ex = (pos.side == 1) ? (bc - inst.exit_slip_pts) : (bc + inst.exit_slip_pts);
                        realize_closed(pos, ex, t, sb, 0);
                        pool[i] = pool.back(); pool.pop_back();
                        continue;
                    }
                    pos.stop_px = (pos.side == 1)
                        ? std::min(new_stop, bc - inst.tick_size)
                        : std::max(new_stop, bc + inst.tick_size);
                }
                ++i;
            }
            ++t;
        }

        if (!pool.empty()) {
            const auto& last_sb = tl.day_last_sym_bar[d];
            size_t end_tl = t_end - 1;
            for (const auto& pos : pool) {
                int32_t lb = last_sb[pos.sym_id];
                if (lb < 0) lb = tl.bars[end_tl].latest_bar[pos.sym_id];
                double mc = tl.mds[pos.sym_id]->bars[lb].c;
                const InstrumentSpec& inst = insts[pos.sym_id];
                double ex = (pos.side == 1) ? (mc - inst.exit_slip_pts) : (mc + inst.exit_slip_pts);
                realize_closed(pos, ex, end_tl, lb, 3);
            }
            pool.clear();
        }

        bool consec = hit_dll_today && prev_dll;
        if (hit_dll_today) res.single_dll_days++;
        if (consec) res.consec_dll_days++;
        prev_dll = hit_dll_today;

        DayRecord dr{};
        dr.cme_day_idx = static_cast<int32_t>(d - d_start);
        dr.cme_yyyymmdd = tl.day_ymds[d];
        dr.day_pnl = day_pnl;
        dr.worst_intra_delta = day_worst_intra;
        dr.trades_cnt = day_trades;
        dr.hit_dll = hit_dll_today;
        dr.consec_dll = consec;
        dr.dll_sess_id = dll_sess;
        for (int s = 0; s < 4; ++s) dr.sess_pnl[s] = sess_pnl[s];
        res.days.push_back(dr);
    }
    res.net_usd = cum_eq;
    return res;
}

struct PreIndexedCombineEvents {
    size_t d_start = 0, d_end = 0;
    std::vector<std::vector<const CausalEntryEvent*>> day_events;
};

inline PreIndexedCombineEvents build_combine_day_events(
    const MasterTimeline& tl, const std::vector<CausalEntryEvent>& events_sorted,
    int32_t min_ymd, int32_t max_ymd)
{
    PreIndexedCombineEvents out;
    if (tl.day_ymds.empty()) return out;
    size_t ds = 0, de = tl.day_ymds.size();
    while (ds < de && tl.day_ymds[ds] < min_ymd) ++ds;
    while (de > ds && tl.day_ymds[de - 1] > max_ymd) --de;
    out.d_start = ds; out.d_end = de;
    if (ds >= de) return out;
    out.day_events.resize(de - ds);
    for (const auto& ev : events_sorted) {
        if (ev.ymd < min_ymd || ev.ymd > max_ymd) continue;
        int32_t tdi = tl.bars[ev.entry_tl].tl_day_idx;
        if (tdi >= static_cast<int32_t>(ds) && tdi < static_cast<int32_t>(de)) {
            out.day_events[static_cast<size_t>(tdi) - ds].push_back(&ev);
        }
    }
    return out;
}

struct CombineStatsV7 {
    CombinePolicyV7 policy;
    int trials = 0, passed = 0;
    double pass_pct = 0, mll_pct = 0, tout_pct = 0, consec_dll_pct = 0;
    double le2_pct = 0, le3_pct = 0, le5_pct = 0, le10_pct = 0;
    double ttp_avg = 0, ttp_p10 = 0, ttp_p20 = 0, ttp_p25 = 0, ttp_p50 = 0;
    double ttp_p75 = 0, ttp_p80 = 0, ttp_p90 = 0, ttp_p95 = 0, ttp_p99 = 0;
    double ddp50 = 0, ddp95 = 0, ddp99 = 0;
    double avg_dll_hits = 0.0;
};

inline CombineStatsV7 eval_simultaneous_combine_v7(
    const CombinePolicyV7& sp,
    const MasterTimeline& tl,
    const std::array<const HtfCausalLevels*, 3>& htf_ptrs,
    const std::array<const std::vector<bool>*, 3>& mr_5m_ptrs,
    const PreIndexedCombineEvents& ev_idx,
    const std::array<InstrumentSpec, 3>& insts,
    bool eod_mll_mode,
    bool enforce_50pct_cons = true,
    int min_pass_days = 2,
    double daily_dll_usd = 1000.0,
    double mll_limit_usd = 2000.0)
{
    CombineStatsV7 cs;
    cs.policy = sp;
    if (ev_idx.d_end < ev_idx.d_start + 180) return cs;
    const size_t n_days = ev_idx.d_end - ev_idx.d_start;

    int trials = 0, passed = 0, fmll = 0, tout = 0, consec_dll_fails = 0;
    int le2 = 0, le3 = 0, le5 = 0, le10 = 0;
    long long total_dll_hits = 0;
    std::vector<double> ttps, tdds;
    ttps.reserve(n_days); tdds.reserve(n_days);

    std::vector<ActiveLegPos> pool;
    pool.reserve(64);
    std::vector<ActiveLegPos> spawned;
    spawned.reserve(4);
    std::vector<uint8_t> group_tp1_hit;
    group_tp1_hit.reserve(64);

    std::array<bool, MAX_LEGS> leg_open{};
    std::array<int32_t, MAX_LEGS> leg_cool_end{};

    for (size_t sd = 0; sd + 180 <= n_days; ++sd) {
        trials++;
        double eq = 0.0, intra_pk = 0.0, eod_pk = 0.0, mdd = 0.0, best_day_pnl = 0.0;
        int outc = 0, du = 0, trial_dll_hits = 0;
        bool prev_day_dll = false, had_consec_dll = false;

        for (size_t off = 0; off < 180; ++off) {
            const size_t d_abs = ev_idx.d_start + sd + off;
            const auto& evs = ev_idx.day_events[sd + off];
            const size_t t_end = tl.day_tl_end[d_abs];
            size_t t = tl.day_tl_begin[d_abs], ev_ptr = 0;

            pool.clear();
            group_tp1_hit.clear();
            leg_open.fill(false);
            leg_cool_end.fill(-1);

            const double day_start = eq;
            double day_pnl = 0.0;
            double day_peak_pnl = 0.0;
            const double dd_start = eod_mll_mode ? std::max(0.0, eod_pk - day_start) : std::max(0.0, intra_pk - day_start);
            const double eff_ds = (sp.ds_alpha < 5.0)
                ? std::min(sp.daily_stop, std::max(sp.ds_floor, sp.ds_alpha * std::max(0.0, sp.m_buf - dd_start)))
                : sp.daily_stop;

            double eff_dcap = sp.dcap;
            if (sp.cons_tp_clamp && enforce_50pct_cons) {
                double room_today = (off == 0)
                    ? 1485.0
                    : ((day_start >= 1510.0) ? std::max(600.0, std::min(sp.dcap, day_start - 5.0)) : 1485.0);
                eff_dcap = std::min(eff_dcap, room_today);
            }

            bool stop_new_entries = false, day_halted = false, dead = false, hit_dll_today = false;
            int consec_losses = 0;
            int64_t last_fam_ts[8] = {-999999999LL, -999999999LL, -999999999LL, -999999999LL,
                                      -999999999LL, -999999999LL, -999999999LL, -999999999LL};
            int8_t last_fam_dir[8] = {};

            auto realize_close = [&](const ActiveLegPos& pos, double exit_px, int32_t x_sb) {
                const InstrumentSpec& inst = insts[static_cast<size_t>(pos.sym_id)];
                double pts = (pos.side == 1) ? (exit_px - pos.entry_px) : (pos.entry_px - exit_px);
                double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                double usd = pts * inst.micro_pv * pos.micros - fee;
                eq += usd;
                day_pnl += usd;
                if (usd > 0.0) consec_losses = 0;
                else consec_losses++;
                day_peak_pnl = std::max(day_peak_pnl, day_pnl);
                if (!eod_mll_mode && eq > intra_pk) intra_pk = eq;

                bool still_open = false;
                for (const auto& op : pool) {
                    if (&op != &pos && op.group_id == pos.group_id) { still_open = true; break; }
                }
                if (!still_open && pos.leg_id >= 0 && static_cast<size_t>(pos.leg_id) < MAX_LEGS) {
                    leg_open[static_cast<size_t>(pos.leg_id)] = false;
                    leg_cool_end[static_cast<size_t>(pos.leg_id)] = x_sb + pos.cooldown_bars;
                }
            };

            auto check_pass_now = [&](double cur_eq, double cur_day_pnl) -> bool {
                int days_el = static_cast<int>(off) + 1;
                if (days_el < min_pass_days || cur_eq < 3000.0) return false;
                if (!enforce_50pct_cons) return true;
                double hypo_best = std::max(best_day_pnl, cur_day_pnl);
                return (hypo_best <= 0.50001 * cur_eq);
            };

            while (t < t_end && !day_halted && !dead) {
                if (pool.empty()) {
                    if (ev_ptr >= evs.size() || stop_new_entries) break;
                    t = std::max(t, evs[ev_ptr]->entry_tl);
                    if (t >= t_end) break;
                }
                const auto& tb = tl.bars[t];
                while (ev_ptr < evs.size() && evs[ev_ptr]->entry_tl < t) ++ev_ptr;

                while (!stop_new_entries && ev_ptr < evs.size() && evs[ev_ptr]->entry_tl == t) {
                    const CausalEntryEvent* ev = evs[ev_ptr++];
                    size_t lid = static_cast<size_t>(std::clamp(ev->leg_id, 0, static_cast<int>(MAX_LEGS - 1)));
                    int32_t sb = tb.bar_idx[static_cast<size_t>(ev->sym_id)];
                    if (sb < 0 || leg_open[lid] || (leg_cool_end[lid] >= 0 && sb <= leg_cool_end[lid])) continue;

                    int open_total = 0, open_sym = 0, cur_m = 0;
                    for (size_t pi = 0; pi < pool.size(); ++pi) {
                        cur_m += pool[pi].micros;
                        bool dup = false;
                        for (size_t pj = 0; pj < pi; ++pj) {
                            if (pool[pj].group_id == pool[pi].group_id) { dup = true; break; }
                        }
                        if (!dup) {
                            open_total++;
                            if (pool[pi].sym_id == ev->sym_id) open_sym++;
                        }
                    }
                    if (open_total >= sp.max_open_total || open_sym >= sp.max_open_per_sym) continue;

                    double open_unr_now = 0.0;
                    for (const auto& op : pool) {
                        int32_t lb = (tb.bar_idx[op.sym_id] >= 0) ? tb.bar_idx[op.sym_id] : tb.latest_bar[op.sym_id];
                        if (lb >= 0) {
                            double mkt_o = (tb.bar_idx[op.sym_id] >= 0)
                                ? static_cast<double>(tl.mds[op.sym_id]->bars[lb].o)
                                : static_cast<double>(tl.mds[op.sym_id]->bars[lb].c);
                            const InstrumentSpec& oinst = insts[op.sym_id];
                            double ex = (op.side == 1) ? (mkt_o - oinst.exit_slip_pts) : (mkt_o + oinst.exit_slip_pts);
                            double fee = resolve_rt_fee_usd(op.micros, false, oinst.fee_rt_micro, oinst.fee_rt_mini);
                            open_unr_now += ((op.side == 1) ? (ex - op.entry_px) : (op.entry_px - ex)) * oinst.micro_pv * op.micros - fee;
                        }
                    }

                    double eq_now = eq + open_unr_now;
                    double dpnl_now = eq_now - day_start;
                    day_peak_pnl = std::max(day_peak_pnl, dpnl_now);
                    if (dpnl_now <= -eff_ds || dpnl_now >= eff_dcap) { stop_new_entries = true; break; }
                    if (sp.prof_lock && day_peak_pnl >= 900.0 && dpnl_now <= day_peak_pnl - 300.0) { stop_new_entries = true; break; }

                    int room = std::max(0, 50 - cur_m);
                    if (room <= 0) continue;

                    double dd_ref = eod_mll_mode ? std::max(0.0, eod_pk - eq_now) : std::max(0.0, intra_pk - eq_now);
                    if (sp.dd_shield_limit > 0.0 && dd_ref >= 0.85 * sp.dd_shield_limit) { stop_new_entries = true; break; }
                    if (sp.tier_dd_gate && dd_ref >= 0.35 * sp.m_buf && !ev->is_tier_s) continue;

                    double cushion = std::max(0.0, sp.m_buf - dd_ref);
                    double r_use = sp.r_min;
                    if (sp.mode == 0) {
                        r_use = std::clamp(sp.m_mult * cushion, sp.r_min, sp.r_max);
                        if (eq_now >= sp.up_thr && dd_ref <= 250.0) r_use = std::max(r_use, sp.up_risk);
                    } else if (sp.mode == 1) {
                        r_use = (dd_ref >= 800.0) ? 125.0 : ((dd_ref >= 400.0) ? 250.0 : ((eq_now >= 750.0) ? 750.0 : 500.0));
                    } else {
                        r_use = sp.m_buf;
                    }

                    if (sp.dd_shield_limit > 0.0 && dd_ref >= 0.55 * sp.dd_shield_limit) {
                        r_use = std::min(r_use, std::max(sp.r_min, 0.30 * std::max(0.0, sp.dd_shield_limit - dd_ref)));
                    }

                    if (sp.confluence_boost && dd_ref <= 250.0) {
                        bool other_same = false;
                        for (int f = 0; f < 8; ++f) {
                            if (f != ev->fam_id && tb.ts_sec - last_fam_ts[f] <= 900 && last_fam_dir[f] == ev->dir) {
                                other_same = true; break;
                            }
                        }
                        if (other_same) r_use *= 1.30;
                    }
                    if (sp.streak_brake && consec_losses >= 2) r_use *= 0.50;

                    double rem_day = eff_dcap - dpnl_now;
                    if (rem_day <= 0.0) { stop_new_entries = true; break; }
                    if (sp.target_cap) {
                        double rem_tgt = std::max(0.0, std::min(rem_day, 3000.0 - eq_now));
                        r_use = std::min(r_use, std::max(sp.r_min, rem_tgt / 1.35));
                    }

                    const InstrumentSpec& inst = insts[static_cast<size_t>(ev->sym_id)];
                    double one_m_risk = ev->sl_pts * inst.micro_pv;
                    if (sp.open_heat_frac < 5.0 && !pool.empty()) {
                        double net_heat = 0.0;
                        for (const auto& op : pool) {
                            double op_risk = op.sl_pts * insts[op.sym_id].micro_pv * op.micros;
                            net_heat += (op.side == ev->dir) ? op_risk : (0.25 * op_risk);
                        }
                        double max_heat = sp.open_heat_frac * std::max(cushion, 200.0);
                        double avail = max_heat - net_heat;
                        if (avail < 0.5 * one_m_risk) continue;
                        r_use = std::min(r_use, avail);
                    }

                    int want = std::clamp(static_cast<int>(std::lround(r_use * ev->kelly_mult / one_m_risk)), 1, 50);
                    int micros = std::min(want, room);
                    double tp_ovr = -1.0;
                    if (sp.cons_tp_clamp && enforce_50pct_cons && !ev->two_leg_split && ev->tp_pts > 0.0) {
                        double one_m_tp = std::max(1.0, (ev->tp_pts - inst.exit_slip_pts) * inst.micro_pv);
                        int max_m_cons = std::max(1, static_cast<int>(std::floor((rem_day + 5.0) / one_m_tp)));
                        micros = std::min(micros, max_m_cons);
                        double fee_m = resolve_rt_fee_usd(micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                        double est_win = (ev->tp_pts - inst.exit_slip_pts) * inst.micro_pv * micros - fee_m;
                        if (est_win > rem_day) {
                            double max_tp = (rem_day + fee_m) / (inst.micro_pv * micros) + inst.exit_slip_pts;
                            tp_ovr = std::max(inst.tick_size * 4.0, max_tp);
                        }
                    }

                    int32_t gid = static_cast<int32_t>(group_tp1_hit.size());
                    if (open_event_into_positions(*ev, tl.mds[ev->sym_id]->bars[sb], sb, micros, inst, gid, spawned, tp_ovr)) {
                        group_tp1_hit.push_back(0);
                        leg_open[lid] = true;
                        int fi = std::clamp(ev->fam_id, 0, 7);
                        last_fam_ts[fi] = tb.ts_sec;
                        last_fam_dir[fi] = ev->dir;
                        for (const auto& p_new : spawned) pool.push_back(p_new);
                    }
                }

                if (pool.empty()) { ++t; continue; }

                double min_real_eq = eq, max_real_eq = eq;

                // Step 3A: Intrabar Stops
                for (size_t i = 0; i < pool.size();) {
                    auto& pos = pool[i];
                    int32_t sb = tb.bar_idx[static_cast<size_t>(pos.sym_id)];
                    if (sb < 0) { ++i; continue; }
                    const auto& bar = tl.mds[pos.sym_id]->bars[sb];
                    const InstrumentSpec& inst = insts[static_cast<size_t>(pos.sym_id)];
                    const double bo = bar.o, bh = bar.h, bl = bar.l;
                    if (pos.is_clk2_eod950 && tl.mds[pos.sym_id]->meta[sb].et_min >= 950) {
                        double ex = (pos.side == 1) ? (bar.c - inst.exit_slip_pts) : (bar.c + inst.exit_slip_pts);
                        realize_close(pos, ex, sb);
                        min_real_eq = std::min(min_real_eq, eq);
                        max_real_eq = std::max(max_real_eq, eq);
                        pool[i] = pool.back(); pool.pop_back();
                        continue;
                    }
                    bool hit_sl = (pos.side == 1) ? (bl <= pos.stop_px) : (bh >= pos.stop_px);
                    if (hit_sl) {
                        double ref = (pos.side == 1) ? ((bo < pos.stop_px) ? bo : pos.stop_px)
                                                     : ((bo > pos.stop_px) ? bo : pos.stop_px);
                        double ex = (pos.side == 1) ? (ref - inst.exit_slip_pts) : (ref + inst.exit_slip_pts);
                        realize_close(pos, ex, sb);
                        min_real_eq = std::min(min_real_eq, eq);
                        max_real_eq = std::max(max_real_eq, eq);
                        pool[i] = pool.back(); pool.pop_back();
                        continue;
                    }
                    ++i;
                }

                // Step 3B: Simultaneous MTM DD, MLL & DLL
                double worst_unr = 0.0, best_unr = 0.0;
                if (!pool.empty()) {
                    double sl_u[3] = {}, sh_u[3] = {};
                    bool has_s[3] = {};
                    for (const auto& pos : pool) {
                        int s = pos.sym_id;
                        int32_t lb = (tb.bar_idx[s] >= 0) ? tb.bar_idx[s] : tb.latest_bar[s];
                        if (lb < 0) continue;
                        has_s[s] = true;
                        const auto& bar = tl.mds[s]->bars[lb];
                        const InstrumentSpec& inst = insts[s];
                        double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                        double pv = inst.micro_pv;
                        auto eval_px = [&](double px) {
                            double ex = (pos.side == 1) ? (px - inst.exit_slip_pts) : (px + inst.exit_slip_pts);
                            double pts = (pos.side == 1) ? (ex - pos.entry_px) : (pos.entry_px - ex);
                            return pts * pv * pos.micros - fee;
                        };
                        double eff_l = (pos.side == -1 && pos.target_px > 0.0) ? std::max<double>(bar.l, pos.target_px) : static_cast<double>(bar.l);
                        double eff_h = (pos.side == 1  && pos.target_px > 0.0) ? std::min<double>(bar.h, pos.target_px) : static_cast<double>(bar.h);
                        sl_u[s] += eval_px(eff_l);
                        sh_u[s] += eval_px(eff_h);
                    }
                    for (int s = 0; s < 3; ++s) {
                        if (!has_s[s]) continue;
                        worst_unr += std::min(sl_u[s], sh_u[s]);
                        best_unr += std::max(sl_u[s], sh_u[s]);
                    }
                }
                double worst_eq_bar = min_real_eq + worst_unr;
                double best_eq_bar  = max_real_eq + best_unr;
                double worst_dpnl = worst_eq_bar - day_start;

                if (!eod_mll_mode) {
                    if (best_eq_bar > intra_pk) intra_pk = best_eq_bar;
                    mdd = std::max(mdd, intra_pk - worst_eq_bar);
                    if (intra_pk - worst_eq_bar >= mll_limit_usd || worst_eq_bar <= -mll_limit_usd) {
                        outc = 1; dead = true; break;
                    }
                    if (worst_dpnl <= -daily_dll_usd || day_pnl <= -daily_dll_usd) {
                        if (!pool.empty()) {
                            double rem_needed = (-daily_dll_usd) - day_pnl;
                            size_t nr = pool.size();
                            for (size_t k = 0; k < nr; ++k) {
                                const auto& pos = pool[k];
                                const InstrumentSpec& inst = insts[pos.sym_id];
                                double tgt_u = rem_needed / static_cast<double>(nr);
                                double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                                double pts_n = (tgt_u + fee) / (inst.micro_pv * pos.micros);
                                double liq_px = (pos.side == 1) ? (pos.entry_px + pts_n) : (pos.entry_px - pts_n);
                                int32_t lb = (tb.bar_idx[pos.sym_id] >= 0) ? tb.bar_idx[pos.sym_id] : tb.latest_bar[pos.sym_id];
                                realize_close(pos, liq_px, lb);
                            }
                            pool.clear();
                        }
                        if (day_pnl > -daily_dll_usd) { day_pnl = -daily_dll_usd; eq = day_start + day_pnl; }
                        hit_dll_today = true; day_halted = true; break;
                    }
                } else {
                    double mll_floor = std::min(0.0, eod_pk - mll_limit_usd);
                    double dll_floor_day = day_start - daily_dll_usd;
                    double eff_worst = std::max(worst_eq_bar, dll_floor_day);
                    if (mll_floor >= dll_floor_day && worst_eq_bar <= mll_floor) {
                        mdd = std::max(mdd, eod_pk - mll_floor);
                        outc = 1; dead = true; break;
                    }
                    mdd = std::max(mdd, std::max(0.0, eod_pk - eff_worst));
                    if (worst_dpnl <= -daily_dll_usd || day_pnl <= -daily_dll_usd) {
                        if (!pool.empty()) {
                            double rem_needed = (-daily_dll_usd) - day_pnl;
                            size_t nr = pool.size();
                            for (size_t k = 0; k < nr; ++k) {
                                const auto& pos = pool[k];
                                const InstrumentSpec& inst = insts[pos.sym_id];
                                double tgt_u = rem_needed / static_cast<double>(nr);
                                double fee = resolve_rt_fee_usd(pos.micros, false, inst.fee_rt_micro, inst.fee_rt_mini);
                                double pts_n = (tgt_u + fee) / (inst.micro_pv * pos.micros);
                                double liq_px = (pos.side == 1) ? (pos.entry_px + pts_n) : (pos.entry_px - pts_n);
                                int32_t lb = (tb.bar_idx[pos.sym_id] >= 0) ? tb.bar_idx[pos.sym_id] : tb.latest_bar[pos.sym_id];
                                realize_close(pos, liq_px, lb);
                            }
                            pool.clear();
                        }
                        day_pnl = std::min(day_pnl, -daily_dll_usd);
                        eq = day_start + day_pnl;
                        hit_dll_today = true; day_halted = true; break;
                    }
                }

                // Step 3C: Intrabar Profit Targets
                for (size_t i = 0; i < pool.size();) {
                    auto& pos = pool[i];
                    int32_t sb = tb.bar_idx[static_cast<size_t>(pos.sym_id)];
                    if (sb < 0 || pos.target_px <= 0.0) { ++i; continue; }
                    const auto& bar = tl.mds[pos.sym_id]->bars[sb];
                    const InstrumentSpec& inst = insts[static_cast<size_t>(pos.sym_id)];
                    bool hit_tp = (pos.side == 1) ? (bar.h >= pos.target_px) : (bar.l <= pos.target_px);
                    if (hit_tp) {
                        double fav = (pos.side == 1) ? (bar.h - pos.entry_px) : (pos.entry_px - bar.l);
                        pos.peak_mfe = std::max(pos.peak_mfe, fav);
                        double ex = (pos.side == 1) ? (pos.target_px - inst.exit_slip_pts) : (pos.target_px + inst.exit_slip_pts);
                        realize_close(pos, ex, sb);
                        if (pos.two_leg_split && pos.sub_leg == 0) group_tp1_hit[pos.group_id] = 1;
                        pool[i] = pool.back(); pool.pop_back();
                        continue;
                    }
                    ++i;
                }

                // Step 3D: Bar-Close Profit Cap, Pass Check, Time/Stag/Ratchets
                if (day_pnl >= eff_dcap) {
                    for (size_t k = 0; k < pool.size(); ++k) {
                        const auto& pos = pool[k];
                        int32_t lb = (tb.bar_idx[pos.sym_id] >= 0) ? tb.bar_idx[pos.sym_id] : tb.latest_bar[pos.sym_id];
                        double mc = tl.mds[pos.sym_id]->bars[lb].c;
                        const InstrumentSpec& inst = insts[pos.sym_id];
                        double ex = (pos.side == 1) ? (mc - inst.exit_slip_pts) : (mc + inst.exit_slip_pts);
                        realize_close(pos, ex, lb);
                    }
                    pool.clear();
                    day_halted = true;
                    if (check_pass_now(eq, day_pnl)) { outc = 2; du = static_cast<int>(off) + 1; dead = true; }
                    break;
                }

                for (size_t i = 0; i < pool.size();) {
                    auto& pos = pool[i];
                    int32_t sb = tb.bar_idx[static_cast<size_t>(pos.sym_id)];
                    if (sb < 0) { ++i; continue; }
                    const auto* md_p = tl.mds[pos.sym_id];
                    const auto& bar = md_p->bars[sb];
                    const InstrumentSpec& inst = insts[pos.sym_id];
                    const double bc = bar.c;

                    if (sb >= pos.end_sym_b) {
                        double ex = (pos.side == 1) ? (bc - inst.exit_slip_pts) : (bc + inst.exit_slip_pts);
                        realize_close(pos, ex, sb);
                        pool[i] = pool.back(); pool.pop_back();
                        continue;
                    }
                    double fav = (pos.side == 1) ? (bar.h - pos.entry_px) : (pos.entry_px - bar.l);
                    if (fav > pos.peak_mfe) pos.peak_mfe = fav;
                    pos.peak_px = (pos.side == 1) ? std::max<double>(pos.peak_px, bar.h) : std::min<double>(pos.peak_px, bar.l);

                    if (pos.stag_bars > 0 && (sb - pos.entry_sym_b) >= pos.stag_bars && pos.peak_mfe < pos.stag_mfe_r * pos.sl_pts) {
                        double cf = pos.side * (bc - pos.entry_px);
                        if (cf < 0.0) {
                            double ex = (pos.side == 1) ? (bc - inst.exit_slip_pts) : (bc + inst.exit_slip_pts);
                            realize_close(pos, ex, sb);
                            pool[i] = pool.back(); pool.pop_back();
                            continue;
                        }
                    }

                    double new_stop = pos.stop_px;
                    bool stop_moved = false;
                    if (pos.be_trig > 0.0 && pos.peak_mfe >= pos.be_trig) {
                        double q = quantize_stop_tick(pos.entry_px + pos.side * pos.be_offset, pos.side, inst.tick_size);
                        if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                    }
                    if (pos.r_trig > 0.0 && pos.peak_mfe >= pos.r_trig) {
                        double lock = std::max(pos.r_trig - pos.r_trail, pos.peak_mfe - pos.r_trail);
                        double q = quantize_stop_tick(pos.entry_px + pos.side * lock, pos.side, inst.tick_size);
                        if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                    }
                    if (pos.is_mr_trail && mr_5m_ptrs[pos.sym_id] && (*mr_5m_ptrs[pos.sym_id])[sb]) {
                        if (pos.mr_trailing) {
                            double raw_s = (pos.side == 1) ? (pos.peak_px - 0.8 * pos.mr_atr5_entry) : (pos.peak_px + 0.8 * pos.mr_atr5_entry);
                            double q = quantize_stop_tick(raw_s, pos.side, inst.tick_size);
                            if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                        } else {
                            if ((pos.side == 1 && bc >= pos.mr_tgt_px) || (pos.side == -1 && bc <= pos.mr_tgt_px)) {
                                pos.mr_trailing = true;
                                double q = quantize_stop_tick(pos.mr_tgt_px, pos.side, inst.tick_size);
                                if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                            }
                        }
                    }
                    if (pos.two_leg_split && pos.sub_leg == 1 && group_tp1_hit[pos.group_id]) {
                        if (!pos.tp1_applied_close) {
                            pos.tp1_applied_close = true;
                            double q = quantize_stop_tick(pos.entry_px + pos.side * inst.tick_size, pos.side, inst.tick_size);
                            if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                        }
                        if (pos.is_stack_htf60 && htf_ptrs[pos.sym_id] && (sb % 60 == 0)) {
                            const auto* hp = htf_ptrs[pos.sym_id];
                            double hpx = (pos.side == 1) ? (hp->prev_low[sb] - inst.tick_size) : (hp->prev_high[sb] + inst.tick_size);
                            double q = quantize_stop_tick(hpx, pos.side, inst.tick_size);
                            if ((pos.side == 1 && q > new_stop) || (pos.side == -1 && q < new_stop)) { new_stop = q; stop_moved = true; }
                        }
                    }
                    if (stop_moved) {
                        if ((pos.side == 1 && bc <= new_stop) || (pos.side == -1 && bc >= new_stop)) {
                            double ex = (pos.side == 1) ? (bc - inst.exit_slip_pts) : (bc + inst.exit_slip_pts);
                            realize_close(pos, ex, sb);
                            pool[i] = pool.back(); pool.pop_back();
                            continue;
                        }
                        pos.stop_px = (pos.side == 1)
                            ? std::min(new_stop, bc - inst.tick_size)
                            : std::max(new_stop, bc + inst.tick_size);
                    }
                    ++i;
                }

                if (check_pass_now(eq, day_pnl)) {
                    for (size_t k = 0; k < pool.size(); ++k) {
                        const auto& pos = pool[k];
                        int32_t lb = (tb.bar_idx[pos.sym_id] >= 0) ? tb.bar_idx[pos.sym_id] : tb.latest_bar[pos.sym_id];
                        double mc = tl.mds[pos.sym_id]->bars[lb].c;
                        const InstrumentSpec& inst = insts[pos.sym_id];
                        double ex = (pos.side == 1) ? (mc - inst.exit_slip_pts) : (mc + inst.exit_slip_pts);
                        realize_close(pos, ex, lb);
                    }
                    pool.clear();
                    if (check_pass_now(eq, day_pnl)) { outc = 2; du = static_cast<int>(off) + 1; dead = true; break; }
                }
                ++t;
            }

            if (!dead && !pool.empty()) {
                const auto& last_sb = tl.day_last_sym_bar[d_abs];
                for (const auto& pos : pool) {
                    int32_t lb = last_sb[pos.sym_id];
                    if (lb < 0) lb = tl.bars[t_end - 1].latest_bar[pos.sym_id];
                    double mc = tl.mds[pos.sym_id]->bars[lb].c;
                    const InstrumentSpec& inst = insts[pos.sym_id];
                    double ex = (pos.side == 1) ? (mc - inst.exit_slip_pts) : (mc + inst.exit_slip_pts);
                    realize_close(pos, ex, lb);
                }
                pool.clear();
            }
            if (dead) {
                if (hit_dll_today) trial_dll_hits++;
                break;
            }
            if (hit_dll_today) {
                trial_dll_hits++;
                if (prev_day_dll) had_consec_dll = true;
            }
            prev_day_dll = hit_dll_today;

            if (day_pnl > best_day_pnl) best_day_pnl = day_pnl;
            if (eod_mll_mode) {
                if (eq > eod_pk) eod_pk = eq;
                double eod_mll_fl = std::min(0.0, eod_pk - mll_limit_usd);
                if (eq <= eod_mll_fl) { mdd = std::max(mdd, eod_pk - eq); outc = 1; break; }
            }
            if (check_pass_now(eq, day_pnl)) { outc = 2; du = static_cast<int>(off) + 1; break; }
        }

        total_dll_hits += trial_dll_hits;
        if (had_consec_dll) consec_dll_fails++;
        if (outc == 2) {
            passed++; ttps.push_back(du);
            if (du <= 2) le2++;
            if (du <= 3) le3++;
            if (du <= 5) le5++;
            if (du <= 10) le10++;
        } else if (outc == 1) fmll++;
        else tout++;
        tdds.push_back(mdd);
    }

    std::sort(tdds.begin(), tdds.end());
    std::sort(ttps.begin(), ttps.end());
    double dt = static_cast<double>(std::max(1, trials));
    cs.trials = trials; cs.passed = passed;
    cs.pass_pct = 100.0 * passed / dt;
    cs.mll_pct = 100.0 * fmll / dt;
    cs.tout_pct = 100.0 * tout / dt;
    cs.consec_dll_pct = 100.0 * consec_dll_fails / dt;
    cs.le2_pct = 100.0 * le2 / dt; cs.le3_pct = 100.0 * le3 / dt;
    cs.le5_pct = 100.0 * le5 / dt; cs.le10_pct = 100.0 * le10 / dt;
    cs.ttp_avg = ttps.empty() ? 0.0 : std::accumulate(ttps.begin(), ttps.end(), 0.0) / ttps.size();
    auto q_ttp = [&](int pct) -> double {
        if (ttps.empty()) return 0.0;
        size_t idx = std::min(ttps.size() - 1, ttps.size() * static_cast<size_t>(pct) / 100);
        return ttps[idx];
    };
    cs.ttp_p10 = q_ttp(10); cs.ttp_p20 = q_ttp(20); cs.ttp_p25 = q_ttp(25);
    cs.ttp_p50 = q_ttp(50);
    cs.ttp_p75 = q_ttp(75); cs.ttp_p80 = q_ttp(80); cs.ttp_p90 = q_ttp(90);
    cs.ttp_p95 = q_ttp(95); cs.ttp_p99 = q_ttp(99);
    size_t ndd = tdds.size();
    cs.ddp50 = ndd ? tdds[ndd * 50 / 100] : 0.0;
    cs.ddp95 = ndd ? tdds[ndd * 95 / 100] : 0.0;
    cs.ddp99 = ndd ? tdds[std::min(ndd - 1, ndd * 99 / 100)] : 0.0;
    cs.avg_dll_hits = total_dll_hits / dt;
    return cs;
}

// ============================================================================
// 5. Portfolio Metrics, Correlation, Stage 10 Noise MC, & Printers
// ============================================================================
struct PortMetrics5C {
    std::string tag;
    int trds = 0;
    double tpd = 0, wr = 0, pf = 0, pf_ex10 = 0, net = 0, curve_dd = 0, bar_dd = 0, rdd = 0;
    double h1 = 0, h2 = 0, mod = 0, sharpe = 0, sortino = 0, psr = 0, dsr = 0, r2 = 0, pos_d_pct = 0, worst_d = 0;
};

inline PortMetrics5C summarize_sim_metrics(const std::string& tag, const SimPortfolioResult& sr, int dsr_trials = 240) {
    PortMetrics5C m{};
    m.tag = tag;
    m.trds = static_cast<int>(sr.trades.size());
    const size_t nd = std::max<size_t>(1, sr.days.size());
    m.tpd = m.trds / static_cast<double>(nd);
    int wins = 0, act_d = 0, pos_d = 0;
    double gp = 0.0, gl = 0.0, worst_d = 0.0;
    std::vector<double> win_vals;
    for (const auto& t : sr.trades) {
        if (t.net_usd > 0.0) { wins++; gp += t.net_usd; win_vals.push_back(t.net_usd); }
        else gl += -t.net_usd;
    }
    std::sort(win_vals.begin(), win_vals.end(), std::greater<double>());
    double top10_w = 0.0;
    for (size_t i = 0; i < std::min<size_t>(10, win_vals.size()); ++i) top10_w += win_vals[i];

    for (const auto& d : sr.days) {
        worst_d = std::min(worst_d, d.day_pnl);
        if (d.trades_cnt > 0) { act_d++; if (d.day_pnl > 0.0) pos_d++; }
    }
    auto sm = u11::detail::compute_statistical_metrics(sr.u11_trades, sr.days, sr.bar_max_dd, dsr_trials);
    m.wr = m.trds > 0 ? (100.0 * wins / m.trds) : 0.0;
    m.pf = gl > 1e-9 ? (gp / gl) : 0.0;
    m.pf_ex10 = gl > 1e-9 ? ((gp - top10_w) / gl) : 0.0;
    m.net = gp - gl;
    m.curve_dd = std::max(1.0, sr.curve_max_dd);
    m.bar_dd = std::max(1.0, sr.bar_max_dd);
    m.rdd = m.net / m.bar_dd;
    m.h1 = sr.h1_net; m.h2 = sr.h2_net; m.mod = sr.mod_net;
    m.sharpe = sm.sharpe_ann; m.sortino = sm.sortino_ann;
    m.psr = sm.psr; m.dsr = sm.dsr; m.r2 = sm.r2_linear;
    m.pos_d_pct = act_d > 0 ? (100.0 * pos_d / act_d) : 0.0;
    m.worst_d = worst_d;
    return m;
}

inline std::vector<std::vector<double>> compute_daily_corr_sim(
    const std::vector<SimPortfolioResult>& sims, size_t n_days)
{
    const size_t n = sims.size();
    std::vector<std::vector<double>> corr(n, std::vector<double>(n, 0.0));
    if (n_days < 2) return corr;
    const double dn = static_cast<double>(n_days);
    for (size_t i = 0; i < n; ++i) {
        corr[i][i] = 1.0;
        for (size_t j = i + 1; j < n; ++j) {
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
            for (size_t d = 0; d < n_days; ++d) {
                double x = sims[i].days[d].day_pnl, y = sims[j].days[d].day_pnl;
                sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y;
            }
            double den = std::sqrt(std::max(0.0, (sxx - sx * sx / dn) * (syy - sy * sy / dn)));
            double c = (den > 1e-9) ? ((sxy - sx * sy / dn) / den) : 0.0;
            corr[i][j] = c; corr[j][i] = c;
        }
    }
    return corr;
}

inline BootstrapMetrics run_multi_asset_noise_mc_30k(
    const std::vector<TradeRecord>& trades,
    const std::vector<int8_t>& syms,
    int32_t total_days,
    const std::array<InstrumentSpec, 3>& insts,
    const EngineConfig& cfg,
    double adverse_drag_pts = 0.0,
    double* mean_noise_sharpe_out = nullptr)
{
    BootstrapMetrics bm{};
    bm.iterations = cfg.mc_iterations;
    if (trades.empty() || cfg.mc_iterations <= 0) return bm;
    const size_t nt = trades.size();
    const int ndays = std::max(1, total_days);
    std::vector<double> final_eq(cfg.mc_iterations, 0.0), max_dds(cfg.mc_iterations, 0.0);
    std::vector<double> mc_sharpes(cfg.mc_iterations, 0.0), mc_pnls(cfg.mc_iterations, 0.0), mc_wrs(cfg.mc_iterations, 0.0);
    long long passes = 0, ruins = 0, prof = 0, pass_2d = 0, pass_5d = 0, combine_pass_cnt = 0, sum_ttp = 0;

    #pragma omp parallel for reduction(+:passes,ruins,prof,pass_2d,pass_5d,combine_pass_cnt,sum_ttp) schedule(static)
    for (int it = 0; it < cfg.mc_iterations; ++it) {
        uint64_t rng = (cfg.mc_seed + 7919ULL) ^ (static_cast<uint64_t>(it + 1) * 0xBF58476D1CE4E5B9ULL);
        auto next_u01 = [&]() -> double {
            rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
            return static_cast<double>((rng * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
        };
        double eq = 0.0, pk = 0.0, mdd = 0.0, cur_day_pnl = 0.0, best_day_pnl = 0.0, sum_d = 0.0, sum_d2 = 0.0;
        long long win_tr = 0; int32_t cur_day = -1; int first_day_idx = trades[0].cme_day_idx, pass_day = 0;
        bool passed_combine = false;
        for (size_t i = 0; i < nt; ++i) {
            double jitter_pts = (next_u01() * 2.0 - 1.0) * 1.7320508 * cfg.noise_jitter_pts - adverse_drag_pts;
            double pv = insts[static_cast<size_t>(syms[i])].micro_pv;
            double usd = trades[i].net_usd + jitter_pts * pv * trades[i].contracts_micros;
            if (usd > 0.0) win_tr++;
            eq += usd;
            if (eq > pk) pk = eq;
            mdd = std::max(mdd, pk - eq);
            int32_t d_idx = trades[i].cme_day_idx;
            if (d_idx != cur_day) {
                if (cur_day >= 0) { sum_d += cur_day_pnl; sum_d2 += cur_day_pnl * cur_day_pnl; best_day_pnl = std::max(best_day_pnl, cur_day_pnl); }
                cur_day = d_idx; cur_day_pnl = usd;
            } else cur_day_pnl += usd;
            if (!passed_combine && eq >= cfg.target_profit_usd) {
                int elapsed = (d_idx - first_day_idx) + 1;
                double hypo_best = std::max(best_day_pnl, cur_day_pnl);
                if (elapsed >= 2 && hypo_best <= cfg.consistency_max_pct * eq) { passed_combine = true; pass_day = elapsed; }
            }
        }
        if (cur_day >= 0) { sum_d += cur_day_pnl; sum_d2 += cur_day_pnl * cur_day_pnl; }
        double sr_it = 0.0;
        if (ndays >= 2) {
            double mean_d = sum_d / static_cast<double>(ndays);
            double sd_d = std::sqrt(std::max(0.0, (sum_d2 - sum_d * mean_d) / static_cast<double>(ndays - 1)));
            sr_it = (sd_d > 1e-12) ? ((mean_d / sd_d) * std::sqrt(252.0)) : 0.0;
        }
        if (eq > 0.0) prof++;
        if (eq >= cfg.target_profit_usd) passes++;
        if (passed_combine) { combine_pass_cnt++; sum_ttp += pass_day; if (pass_day <= 2) pass_2d++; if (pass_day <= 5) pass_5d++; }
        if (mdd >= cfg.mll_limit_usd) ruins++;
        final_eq[it] = eq; max_dds[it] = mdd; mc_sharpes[it] = sr_it; mc_pnls[it] = eq;
        mc_wrs[it] = 100.0 * static_cast<double>(win_tr) / static_cast<double>(nt);
    }
    const double nit = static_cast<double>(cfg.mc_iterations);
    bm.pass_rate_pct = 100.0 * passes / nit; bm.ruin_rate_pct = 100.0 * ruins / nit;
    bm.profitable_pct = 100.0 * prof / nit; bm.pass_2d_pct = 100.0 * pass_2d / nit; bm.pass_5d_pct = 100.0 * pass_5d / nit;
    bm.mean_ttp = (combine_pass_cnt > 0) ? (static_cast<double>(sum_ttp) / combine_pass_cnt) : 0.0;
    if (mean_noise_sharpe_out) *mean_noise_sharpe_out = std::accumulate(mc_sharpes.begin(), mc_sharpes.end(), 0.0) / nit;
    u11::calc_ci95(std::move(mc_sharpes), bm.ci95_sharpe_low, bm.ci95_sharpe_high);
    u11::calc_ci95(std::move(mc_pnls), bm.ci95_pnl_low, bm.ci95_pnl_high);
    u11::calc_ci95(std::move(mc_wrs), bm.ci95_wr_low, bm.ci95_wr_high);
    bm.final_equity = calc_percentiles(std::move(final_eq));
    bm.max_dd = calc_percentiles(std::move(max_dds));
    return bm;
}

inline void print_port_tbl(const std::string& hdr, const std::vector<PortMetrics5C>& rows) {
    std::cout << "\n=== " << hdr << " ===\n";
    std::cout << std::left << std::setw(34) << "Strategy / Split" << std::right
              << std::setw(6) << "Trds" << std::setw(6) << "T/D" << std::setw(6) << "WR%"
              << std::setw(6) << "PF" << std::setw(6) << "PFx10"
              << std::setw(9) << "Net$" << std::setw(7) << "CrvDD" << std::setw(7) << "BarDD"
              << std::setw(7) << "RetDD" << std::setw(6) << "Shrp" << std::setw(6) << "Sort"
              << std::setw(6) << "PSR" << std::setw(6) << "DSR" << std::setw(6) << "R2"
              << std::setw(8) << "WorstD$" << "\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(34) << r.tag << std::right << std::setw(6) << r.trds
                  << std::setw(6) << std::fixed << std::setprecision(2) << r.tpd
                  << std::setw(6) << std::setprecision(1) << r.wr
                  << std::setw(6) << std::setprecision(2) << r.pf
                  << std::setw(6) << std::setprecision(2) << r.pf_ex10
                  << std::setw(9) << std::setprecision(0) << r.net
                  << std::setw(7) << r.curve_dd << std::setw(7) << r.bar_dd
                  << std::setw(7) << std::setprecision(2) << r.rdd
                  << std::setw(6) << r.sharpe << std::setw(6) << r.sortino
                  << std::setw(6) << r.psr << std::setw(6) << r.dsr << std::setw(6) << r.r2
                  << std::setw(8) << std::setprecision(0) << r.worst_d << "\n";
    }
}

inline void print_combine_row_v7(const CombineStatsV7& c) {
    std::cout << "  " << std::left << std::setw(42) << c.policy.label
              << " | Pass=" << std::fixed << std::setprecision(2) << std::setw(6) << c.pass_pct << "%"
              << " MLL=" << std::setw(5) << c.mll_pct << "%"
              << " CDLL=" << std::setw(5) << c.consec_dll_pct << "%"
              << " Tout=" << std::setw(5) << c.tout_pct << "%"
              << " | <=2d=" << std::setw(5) << c.le2_pct << "% <=3d=" << std::setw(5) << c.le3_pct
              << "% <=5d=" << std::setw(5) << c.le5_pct << "% <=10d=" << std::setw(5) << c.le10_pct << "%"
              << " | TTP=" << std::setw(5) << c.ttp_avg << "/" << std::setprecision(0) << std::setw(3) << c.ttp_p50 << "d"
              << " | DD:p50/95/99=$" << std::setw(4) << c.ddp50 << "/$" << std::setw(4) << c.ddp95 << "/$" << std::setw(4) << c.ddp99 << "\n";
}

// ============================================================================
// 6. Built-In 8+ Adversarial Bug Immunity Tests (Including Concurrency & ConsLock)
// ============================================================================
inline RawBar mkbar(int64_t ts, double o, double h, double l, double c, double v = 500.0) {
    RawBar b{}; b.ts_sec = ts;
    b.o = static_cast<float>(o); b.h = static_cast<float>(h); b.l = static_cast<float>(l); b.c = static_cast<float>(c); b.v = static_cast<float>(v);
    return b;
}

inline void run_8_adversarial_bug_tests_v7() {
    std::cout << "=== SECTION 0A: WAVE 7 BUILT-IN 8 ADVERSARIAL BUG TESTS ===" << std::endl;
    constexpr int64_t NY24 = 1704724200LL;
    std::array<InstrumentSpec, 3> insts = {realistic_spec("NQ"), realistic_spec("ES"), realistic_spec("YM")};
    std::array<const HtfCausalLevels*, 3> no_htf = {nullptr, nullptr, nullptr};
    std::array<const std::vector<bool>*, 3> no_mr = {nullptr, nullptr, nullptr};

    // Test 1 + Test 8A (Simultaneous Intrabar DLL vs Concurrency Mutex)
    {
        std::vector<RawBar> nq_b, es_b, ym_b;
        for (int i = 0; i < 40; ++i) {
            double o_nq = 20000.0, h_nq = 20001.0, l_nq = 19999.0, c_nq = 20000.0;
            double o_es = 5000.0,  h_es = 5001.0,  l_es = 4999.0,  c_es = 5000.0;
            if (i == 26) { l_nq = 19980.0; l_es = 4991.0; }
            if (i == 35) { h_nq = 20050.0; c_nq = 20045.0; }
            nq_b.push_back(mkbar(NY24 + i * 60, o_nq, h_nq, l_nq, c_nq));
            es_b.push_back(mkbar(NY24 + i * 60, o_es, h_es, l_es, c_es));
            ym_b.push_back(mkbar(NY24 + i * 60, 40000.0, 40001.0, 39999.0, 40000.0));
        }
        PrecomputedMarketData md_n, md_e, md_y;
        assert(md_n.init_from_bars(std::move(nq_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        assert(md_e.init_from_bars(std::move(es_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        assert(md_y.init_from_bars(std::move(ym_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        MasterTimeline tl = build_master_timeline({&md_n, &md_e, &md_y});

        std::vector<CausalEntryEvent> evs(2);
        evs[0].ymd = tl.day_ymds[0]; evs[0].year = 2024; evs[0].entry_tl = static_cast<size_t>(tl.b_to_tl[0][21]);
        evs[0].sym_id = 0; evs[0].dir = 1; evs[0].leg_id = 0; evs[0].sl_pts = 100.0; evs[0].tp_pts = 34.0; evs[0].max_hold_bars = 30;
        evs[1].ymd = tl.day_ymds[0]; evs[1].year = 2024; evs[1].entry_tl = static_cast<size_t>(tl.b_to_tl[1][25]);
        evs[1].sym_id = 1; evs[1].dir = 1; evs[1].leg_id = 1; evs[1].sl_pts = 50.0; evs[1].tp_pts = 20.0; evs[1].max_hold_bars = 30;

        auto sim = run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20240101, 20241231, 3000.0, false, true, 1000.0, 1500.0);
        assert(sim.single_dll_days == 1);
        assert(sim.trades.size() == 2);
        assert(sim.trades[0].exit_tl == static_cast<size_t>(tl.b_to_tl[0][26]) && sim.trades[1].exit_tl == static_cast<size_t>(tl.b_to_tl[0][26]));
        assert(std::abs(sim.net_usd - (-1000.0)) < 1.0);
        std::cout << "  [PASS] Test 1 (Anti-Bug-#1 Simultaneous Intrabar DLL & Zero Lookahead): DLL at bar 26, Net=$" << sim.net_usd << "\n";

        auto sim_mx = run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20240101, 20241231, 3000.0, false, true, 1000.0, 1500.0, 1, 1);
        assert(sim_mx.single_dll_days == 0);
        assert(sim_mx.trades.size() == 1 && sim_mx.net_usd > 900.0);
        std::cout << "  [PASS] Test 8A (Anti-MTM-Stacking Concurrency Mutex max_open=1): 2nd leg blocked, zero DLL, Net=$" << sim_mx.net_usd << "\n";
    }

    // Tests 2-6
    {
        std::vector<RawBar> nq_b, es_b, ym_b;
        for (int i = 0; i < 60; ++i) {
            nq_b.push_back(mkbar(NY24 + i * 60, 20000, 20001, 19999, 20000));
            es_b.push_back(mkbar(NY24 + i * 60, 5000, 5001, 4999, 5000));
            ym_b.push_back(mkbar(NY24 + i * 60, 40000, 40001, 39999, 40000));
        }
        nq_b[21] = mkbar(NY24 + 21 * 60, 20000, 20004, 19998, 20002);
        nq_b[25] = mkbar(NY24 + 25 * 60, 20000, 20030, 19988, 20028);
        nq_b[30] = mkbar(NY24 + 30 * 60, 20000, 20015, 20000, 20012);
        nq_b[35] = mkbar(NY24 + 35 * 60, 20000, 20002, 19999, 20001);
        nq_b[36] = mkbar(NY24 + 36 * 60, 19980, 19982, 19978, 19981);
        nq_b[40] = mkbar(NY24 + 40 * 60, 20000, 20015.5, 20000, 20010);

        PrecomputedMarketData md_n, md_e, md_y;
        assert(md_n.init_from_bars(std::move(nq_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        assert(md_e.init_from_bars(std::move(es_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        assert(md_y.init_from_bars(std::move(ym_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        MasterTimeline tl = build_master_timeline({&md_n, &md_e, &md_y});

        double fpx = 0.0;
        assert(check_causal_entry_fill(1, 1, 20002.0, md_n.bars[21], 0.50, fpx) && std::abs(fpx - 20002.5) < 1e-9);
        assert(!check_causal_entry_fill(1, 1, 20006.0, md_n.bars[21], 0.50, fpx));
        assert(check_causal_entry_fill(2, 1, 19999.0, md_n.bars[21], 0.50, fpx) && std::abs(fpx - 19999.5) < 1e-9);
        assert(!check_causal_entry_fill(2, 1, 19995.0, md_n.bars[21], 0.50, fpx));
        std::cout << "  [PASS] Test 2 (Anti-Bug-#2 order_type 1/2 Stop vs Pullback + Entry Slip): Exact\n";

        std::vector<CausalEntryEvent> ev3(1);
        ev3[0].ymd = tl.day_ymds[0]; ev3[0].year = 2024; ev3[0].entry_tl = static_cast<size_t>(tl.b_to_tl[0][25]);
        ev3[0].sym_id = 0; ev3[0].dir = 1; ev3[0].leg_id = 0; ev3[0].two_leg_split = true; ev3[0].def_m_safe = 5;
        ev3[0].sl_pts = 10.0; ev3[0].tp1_pts = 8.5; ev3[0].tp_pts = 80.0; ev3[0].max_hold_bars = 10;
        auto s3 = run_simultaneous_portfolio_sim(tl, no_htf, no_mr, ev3, insts, 20240101, 20241231, 200.0, true, false);
        assert(s3.trades.size() == 2 && s3.trades[0].exit_tl == static_cast<size_t>(tl.b_to_tl[0][25]) && s3.trades[1].exit_tl == static_cast<size_t>(tl.b_to_tl[0][25]));
        std::cout << "  [PASS] Test 3 (Anti-Bug-#3 2-Leg Entry-Bar SL): Both sub-legs stopped at eb Net=$" << s3.net_usd << "\n";

        std::vector<CausalEntryEvent> ev4(1);
        ev4[0].ymd = tl.day_ymds[0]; ev4[0].year = 2024; ev4[0].entry_tl = static_cast<size_t>(tl.b_to_tl[0][30]);
        ev4[0].sym_id = 0; ev4[0].dir = 1; ev4[0].leg_id = 0; ev4[0].sl_pts = 20.0; ev4[0].tp_pts = 10.0; ev4[0].max_hold_bars = 5;
        auto s4 = run_simultaneous_portfolio_sim(tl, no_htf, no_mr, ev4, insts, 20240101, 20241231, 200.0, false, false);
        assert(s4.trades.size() == 1 && std::abs(s4.net_usd - 88.0) < 1e-6);
        std::cout << "  [PASS] Test 4 (Anti-Bug-#4 Mandatory TP Exit Slip): 5M +10p TP = $" << s4.net_usd << "\n";

        auto s5 = simulate_single_bar_seq_bugfree(md_n, 35, 1, 20000.50, 10.0, 50.0, 5, 0,0,0,0, 0,0.35, false, false, 0,0, nullptr, insts[0]);
        assert(s5.exit_b == 36 && std::abs(s5.raw_pts_net - (-21.0)) < 1e-6);
        std::cout << "  [PASS] Test 5 (Anti-Bug-#5 Gap-Through-Stop Open Penalty): Fill at open-slip = " << s5.raw_pts_net << " pts\n";

        auto s6 = simulate_single_bar_seq_bugfree(md_n, 40, 1, 20000.50, 20.0, 50.0, 5, 8.0, 2.0, 0,0, 0,0.35, false, false, 0,0, nullptr, insts[0]);
        assert(s6.exit_b == 40 && std::abs(s6.raw_pts_net - 9.0) < 1e-6);
        std::cout << "  [PASS] Test 6 (Anti-Bug-#6 BUG-20 Interbar Ratchet Clamp): Immediate close at bar_c-slip = +" << s6.raw_pts_net << " pts\n";
    }

    // Test 7 (50% Cons Target Escalation) + Test 8B (Causal Consistency-Locking TP Clamp)
    {
        std::vector<RawBar> nq_b, es_b, ym_b;
        for (int d = 0; d < 180; ++d) {
            int64_t db = NY24 + static_cast<int64_t>(d) * 86400LL;
            for (int m = 0; m < 10; ++m) {
                double o = 20000, h = 20001, l = 20000, c = 20000;
                if (d == 0 && m == 2) { h = 20040; c = 20040; }
                else if (d == 0 && m == 3) { o = 20040; h = 20085; l = 20040; c = 20082; }
                else if (d == 1 && m == 2) { h = 20042; c = 20040; }
                else if (d >= 2 && m == 2) { h = 20025; c = 20020; }
                nq_b.push_back(mkbar(db + m * 60, o, h, l, c));
                es_b.push_back(mkbar(db + m * 60, 5000, 5001, 4999, 5000));
                ym_b.push_back(mkbar(db + m * 60, 40000, 40001, 39999, 40000));
            }
        }
        PrecomputedMarketData md_n, md_e, md_y;
        assert(md_n.init_from_bars(std::move(nq_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        assert(md_e.init_from_bars(std::move(es_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        assert(md_y.init_from_bars(std::move(ym_b), true, 0, CANONICAL_IS_END_YYYYMMDD));
        MasterTimeline tl = build_master_timeline({&md_n, &md_e, &md_y});

        std::vector<CausalEntryEvent> ev7;
        for (int d = 0; d < 5; ++d) {
            size_t b = static_cast<size_t>(d * 10 + 2);
            CausalEntryEvent e{};
            e.ymd = tl.day_ymds[d]; e.year = e.ymd / 10000; e.entry_tl = static_cast<size_t>(tl.b_to_tl[0][b]);
            e.sym_id = 0; e.dir = 1; e.leg_id = 0; e.sl_pts = 10.0;
            e.tp_pts = (d == 0) ? 61.0 : ((d == 1) ? 41.0 : 21.0);
            e.max_hold_bars = 5;
            ev7.push_back(e);
        }
        auto ev_idx = build_combine_day_events(tl, ev7, tl.day_ymds.front(), tl.day_ymds.back());
        CombinePolicyV7 flat_noclamp{"FLAT300_NC", 2, 300.0, 0.0, 300.0, 300.0, 9999.0, 300.0, 900.0, 900.0, 99.0, 99.0, 5000.0, false, false, false, false, false, 4, 2, false, 0.0};
        auto c_cons = eval_simultaneous_combine_v7(flat_noclamp, tl, no_htf, no_mr, ev_idx, insts, false, true, 2);
        auto c_nocons = eval_simultaneous_combine_v7(flat_noclamp, tl, no_htf, no_mr, ev_idx, insts, false, false, 2);
        if (!(c_nocons.trials == 1 && c_cons.trials == 1 && c_nocons.ttp_p50 == 2.0 && c_cons.ttp_p50 == 3.0)) {
            std::cerr << "Test 7 FAIL diag: nocons(trials=" << c_nocons.trials << ", pass=" << c_nocons.pass_pct
                      << ", mll=" << c_nocons.mll_pct << ", tout=" << c_nocons.tout_pct << ", ttp=" << c_nocons.ttp_p50
                      << ") cons(trials=" << c_cons.trials << ", pass=" << c_cons.pass_pct
                      << ", mll=" << c_cons.mll_pct << ", tout=" << c_cons.tout_pct << ", ttp=" << c_cons.ttp_p50 << ")\n";
        }
        assert(c_nocons.trials == 1 && c_cons.trials == 1 && c_nocons.ttp_p50 == 2.0 && c_cons.ttp_p50 == 3.0);
        std::cout << "  [PASS] Test 7 (Anti-Bug-#7 50% Consistency Target Escalation): NoCons TTP=" << c_nocons.ttp_p50 << "d vs Exact 50% Cons TTP=" << c_cons.ttp_p50 << "d\n";

        // Test 8B: Day 0 raw TP=80p (+$2,373.50 without clamp -> escalates target to $4,750+ so fails to pass in 3d!),
        // vs cons_tp_clamp=true -> clamps Day 0 <= $1,485, Day 1=+$1,203.50, Day 2=+$603.50 => passes Day 3!
        std::vector<CausalEntryEvent> ev8b(ev7.begin(), ev7.begin() + 3);
        ev8b[0].tp_pts = 80.0;
        auto ev_idx8b = build_combine_day_events(tl, ev8b, tl.day_ymds.front(), tl.day_ymds.back());
        auto c_unclamped = eval_simultaneous_combine_v7(flat_noclamp, tl, no_htf, no_mr, ev_idx8b, insts, false, true, 2);
        CombinePolicyV7 flat_clamp = flat_noclamp;
        flat_clamp.cons_tp_clamp = true;
        auto c_clamped = eval_simultaneous_combine_v7(flat_clamp, tl, no_htf, no_mr, ev_idx8b, insts, false, true, 2);
        if (!(c_unclamped.passed == 0 && c_clamped.passed == 1 && c_clamped.ttp_p50 == 3.0)) {
            std::cerr << "Test 8B FAIL diag: unclamped(passed=" << c_unclamped.passed << ", mll=" << c_unclamped.mll_pct
                      << ", ttp=" << c_unclamped.ttp_p50 << ") clamped(passed=" << c_clamped.passed
                      << ", mll=" << c_clamped.mll_pct << ", ttp=" << c_clamped.ttp_p50 << ")\n";
        }
        assert(c_unclamped.passed == 0 && c_clamped.passed == 1 && c_clamped.ttp_p50 == 3.0);
        std::cout << "  [PASS] Test 8B (Causal Consistency-Locking TP Clamp): Unclamped Pass=" << c_unclamped.pass_pct
                  << "% vs Causal ConsClamp Pass=" << c_clamped.pass_pct << "% TTP=" << c_clamped.ttp_p50 << "d" << std::endl;
    }
}

} // namespace w7
