#include "wave7_zero_bug_engine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace w7;

// Wave 18: Session-Relative Range & Volatility-Normalized Overnight/ES/YM Discovery
// mode 0: Session-Range Extreme Fade (MRT) - fades stretched moves when sess_rng / atr60 >= stretch_th
// mode 1: Volatility-Expansion Continuation (SPL60) - enters impulse breakouts when rng15 >= exp_th * atr15 & sess_rng <= cap * atr60
inline std::vector<CausalEntryEvent> extract_w18_cell(
    int mode, double th1, double th2, double sl_mult, int8_t target_sess, int8_t sym_id,
    const PrecomputedMarketData& md, const MasterTimeline& tl, const InstrumentSpec& inst)
{
    std::vector<CausalEntryEvent> out;
    const size_t N = md.n_bars;
    if (N < 500) return out;
    const double tick = inst.tick_size;

    int64_t cb5 = -1, cb15 = -1, cb60 = -1;
    double c5_o = md.bars[0].o, c5_h = md.bars[0].h, c5_l = md.bars[0].l, c5_c = md.bars[0].c;
    double c15_o = c5_o, c15_h = c5_h, c15_l = c5_l, c15_c = c5_c;
    double c60_h = c5_h, c60_l = c5_l, c60_c = c5_c;
    double prev5_c = c5_c, prev15_c = c5_c, prev60_c = c5_c;
    double atr5 = tick * 8.0, atr15 = tick * 12.0, atr60 = tick * 24.0;
    std::deque<double> tr5, tr15, tr60;

    int8_t prev_sess = -1;
    double sess_hi = c5_h, sess_lo = c5_l, sess_o = c5_o;
    int sess_bars = 0;
    int32_t last_entry_b = -100;

    for (size_t b = 0; b + 1 < N; ++b) {
        const auto& rb = md.bars[b]; const auto& m = md.meta[b];
        double bo = std::max(tick, static_cast<double>(rb.o));
        double bh = std::max(bo, static_cast<double>(rb.h));
        double bl = std::min(bo, std::max(tick, static_cast<double>(rb.l)));
        double bc = std::max(tick, static_cast<double>(rb.c));

        if (m.sess_id != prev_sess) { prev_sess = m.sess_id; sess_o = bo; sess_hi = bh; sess_lo = bl; sess_bars = 0; }
        else { sess_hi = std::max(sess_hi, bh); sess_lo = std::min(sess_lo, bl); }

        int64_t b5 = (static_cast<int64_t>(m.cme_day_idx) * 1440LL + m.et_min) / 5LL;
        if (b5 != cb5) { cb5 = b5; c5_o = bo; c5_h = bh; c5_l = bl; c5_c = bc; }
        else { c5_h = std::max(c5_h, bh); c5_l = std::min(c5_l, bl); c5_c = bc; }

        int64_t b15 = (static_cast<int64_t>(m.cme_day_idx) * 1440LL + m.et_min) / 15LL;
        if (b15 != cb15) { cb15 = b15; c15_o = bo; c15_h = bh; c15_l = bl; c15_c = bc; }
        else { c15_h = std::max(c15_h, bh); c15_l = std::min(c15_l, bl); c15_c = bc; }

        int64_t b60 = (static_cast<int64_t>(m.cme_day_idx) * 1440LL + m.et_min) / 60LL;
        if (b60 != cb60) { cb60 = b60; c60_h = bh; c60_l = bl; c60_c = bc; }
        else { c60_h = std::max(c60_h, bh); c60_l = std::min(c60_l, bl); c60_c = bc; }

        const auto& mn = md.meta[b + 1];
        if ((static_cast<int64_t>(mn.cme_day_idx) * 1440LL + mn.et_min) / 5LL == cb5) continue;

        ++sess_bars;
        double t5 = std::max({c5_h - c5_l, std::abs(c5_h - prev5_c), std::abs(c5_l - prev5_c)});
        tr5.push_back(t5); if (tr5.size() > 14) tr5.pop_front();
        double s5 = 0; for (double x : tr5) s5 += x; atr5 = std::max(tick * 2.0, s5 / tr5.size()); prev5_c = c5_c;

        bool cl15 = ((static_cast<int64_t>(mn.cme_day_idx) * 1440LL + mn.et_min) / 15LL != cb15);
        if (cl15) {
            double t15 = std::max({c15_h - c15_l, std::abs(c15_h - prev15_c), std::abs(c15_l - prev15_c)});
            tr15.push_back(t15); if (tr15.size() > 14) tr15.pop_front();
            double s15 = 0; for (double x : tr15) s15 += x; atr15 = std::max(tick * 3.0, s15 / tr15.size()); prev15_c = c15_c;
        }
        bool cl60 = ((static_cast<int64_t>(mn.cme_day_idx) * 1440LL + mn.et_min) / 60LL != cb60);
        if (cl60) {
            double t60 = std::max({c60_h - c60_l, std::abs(c60_h - prev60_c), std::abs(c60_l - prev60_c)});
            tr60.push_back(t60); if (tr60.size() > 14) tr60.pop_front();
            double s60 = 0; for (double x : tr60) s60 += x; atr60 = std::max(tick * 4.0, s60 / tr60.size()); prev60_c = c60_c;
        }

        int8_t dir = 0;
        double sess_rng = sess_hi - sess_lo;
        if (mode == 0) {
            // th1 = stretch vs atr60, th2 = bar5 body/rng reversal
            if (sess_bars >= 6 && sess_rng >= th1 * atr60) {
                double r5 = std::max(tick, c5_h - c5_l);
                if (c5_c <= sess_lo + 0.20 * sess_rng && (c5_c - c5_o) >= th2 * r5) dir = 1;
                else if (c5_c >= sess_hi - 0.20 * sess_rng && (c5_o - c5_c) >= th2 * r5) dir = -1;
            }
        } else {
            // th1 = 15m impulse vs atr15, th2 = max sess_rng/atr60
            if (cl15 && sess_bars >= 4 && sess_rng <= th2 * atr60) {
                double r15 = std::max(tick, c15_h - c15_l);
                if (r15 >= th1 * atr15) {
                    if (c15_c - c15_o > 0.55 * r15) dir = 1;
                    else if (c15_o - c15_c > 0.55 * r15) dir = -1;
                }
            }
        }

        if (dir == 0 || mn.cme_day_idx != m.cme_day_idx || mn.sess_id != target_sess) continue;
        int32_t ymd = md.day_yyyymmdd[static_cast<size_t>(mn.cme_day_idx)];
        if (ymd < 20180101 || ymd > CANONICAL_IS_END_YYYYMMDD) continue;
        int32_t tl_i = tl.b_to_tl[sym_id][b + 1];
        if (tl_i < 0 || static_cast<int32_t>(b) - last_entry_b < 8) continue;
        last_entry_b = static_cast<int32_t>(b);

        CausalEntryEvent ev{};
        ev.ymd = ymd; ev.year = ymd / 10000; ev.entry_tl = static_cast<size_t>(tl_i);
        ev.sess_id = mn.sess_id; ev.sym_id = sym_id; ev.dir = dir;
        ev.leg_id = 990 + mode; ev.fam_id = 18; ev.is_tier_s = true;
        ev.sl_pts = std::max(tick * 6.0, sl_mult * atr5);
        if (mode == 0) {
            ev.is_mr_trail = true; ev.mr_atr5_entry = atr5;
            ev.mr_tgt_px = 0.5 * (sess_hi + sess_lo);
            ev.tp_pts = 2.5 * ev.sl_pts; ev.max_hold_bars = 120;
        } else {
            ev.two_leg_split = true; ev.is_stack_htf60 = true;
            ev.tp1_pts = 1.5 * ev.sl_pts; ev.tp_pts = 6.0 * ev.sl_pts; ev.max_hold_bars = 360;
        }
        ev.cooldown_bars = 5; ev.kelly_mult = 1.0;
        out.push_back(ev);
    }
    return out;
}

int main(int argc, char** argv) {
    int cell_id = 0;
    std::string data_dir = "/tmp/market_data";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--cell-id") cell_id = std::stoi(argv[i + 1]);
        else if (std::string(argv[i]) == "--data-dir") data_dir = argv[i + 1];
    }
    int8_t sym = static_cast<int8_t>(cell_id / 4);
    int8_t sess = static_cast<int8_t>(cell_id % 4);
    std::array<InstrumentSpec, 3> insts = {realistic_spec("NQ"), realistic_spec("ES"), realistic_spec("YM")};
    std::array<InstrumentSpec, 3> pess_insts = {pessimistic_spec("NQ"), pessimistic_spec("ES"), pessimistic_spec("YM")};
    PrecomputedMarketData nq, es, ym;
    if (!nq.load_binary_auto(data_dir + "/nq_bars_10yr.bin", true, 0, CANONICAL_IS_END_YYYYMMDD) ||
        !es.load_binary_auto(data_dir + "/es_1min.bin", true, 0, CANONICAL_IS_END_YYYYMMDD) ||
        !ym.load_binary_auto(data_dir + "/ym_1min.bin", true, 0, CANONICAL_IS_END_YYYYMMDD)) return 1;
    std::array<const PrecomputedMarketData*, 3> mds = {&nq, &es, &ym};
    MasterTimeline tl = build_master_timeline(mds);

    std::array<std::vector<bool>, 3> is_5m;
    for (int s = 0; s < 3; ++s) {
        is_5m[s].assign(mds[s]->n_bars, false);
        for (size_t b = 0; b + 1 < mds[s]->n_bars; ++b) {
            int64_t b1 = (static_cast<int64_t>(mds[s]->meta[b].cme_day_idx) * 1440LL + mds[s]->meta[b].et_min) / 5LL;
            int64_t b2 = (static_cast<int64_t>(mds[s]->meta[b + 1].cme_day_idx) * 1440LL + mds[s]->meta[b + 1].et_min) / 5LL;
            if (b1 != b2) is_5m[s][b] = true;
        }
    }
    std::array<const HtfCausalLevels*, 3> no_htf = {nullptr, nullptr, nullptr};
    std::array<const std::vector<bool>*, 3> mr_5m_ptrs = {&is_5m[0], &is_5m[1], &is_5m[2]};

    std::cout << "=== WAVE 18 SESSION-RANGE & VOLATILITY EXPANSION (SHARD " << cell_id << ": " << id_to_sym(sym) << " | " << sess_name(sess) << ") ===" << std::endl;
    struct GridPt { int mode; double th1, th2, sl; PortMetrics5C h1, h2, tr; };
    std::vector<GridPt> pts;
    for (double t1 : {1.0, 1.5, 2.0})
        for (double t2 : {0.35, 0.55})
            for (double sl : {1.1, 1.5})
                pts.push_back({0, t1, t2, sl, {}, {}, {}});
    for (double t1 : {1.3, 1.7})
        for (double t2 : {2.0, 3.0})
            for (double sl : {1.1, 1.5})
                pts.push_back({1, t1, t2, sl, {}, {}, {}});

    #pragma omp parallel for schedule(dynamic, 1)
    for (size_t i = 0; i < pts.size(); ++i) {
        auto evs = extract_w18_cell(pts[i].mode, pts[i].th1, pts[i].th2, pts[i].sl, sess, sym, *mds[sym], tl, insts[sym]);
        pts[i].h1 = summarize_sim_metrics("H1", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20180101, 20191231, 200.0, false, false), 9);
        pts[i].h2 = summarize_sim_metrics("H2", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20200101, 20211231, 200.0, false, false), 9);
        pts[i].tr = summarize_sim_metrics("TR", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20180101, 20211231, 200.0, false, false), 9);
    }

    int pass_cnt = 0; int best_i = -1; double best_net = -1e9;
    for (size_t i = 0; i < pts.size(); ++i) {
        bool ok = (pts[i].tr.trds >= 60 && pts[i].h1.trds >= 20 && pts[i].h2.trds >= 20 &&
                   pts[i].h1.pf >= 1.03 && pts[i].h2.pf >= 1.03 && pts[i].tr.pf >= 1.12);
        if (ok) { pass_cnt++; if (pts[i].tr.net > best_net) { best_net = pts[i].tr.net; best_i = static_cast<int>(i); } }
    }
    std::cout << "  TRAIN Plateau Pass=" << pass_cnt << "/" << pts.size() << " (" << (pass_cnt * 100 / pts.size()) << "%)" << std::endl;
    if (best_i >= 0) {
        auto& b = pts[best_i];
        auto evs = extract_w18_cell(b.mode, b.th1, b.th2, b.sl, sess, sym, *mds[sym], tl, insts[sym]);
        auto oo = summarize_sim_metrics("OO", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20220101, 20251231, 200.0, false, false), 9);
        auto pe = summarize_sim_metrics("PE", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, pess_insts, 20180101, 20251231, 200.0, false, false), 9);
        EngineConfig cfg{}; double nsh = 0.0;
        auto sr8 = run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20180101, 20251231, 200.0, false, false);
        auto bm = run_multi_asset_noise_mc_30k(sr8.u11_trades, sr8.u11_trade_syms, static_cast<int32_t>(sr8.days.size()), insts, cfg, 0.0, &nsh);
        std::cout << "  BEST(m=" << b.mode << ",t1=" << b.th1 << ",t2=" << b.th2 << ",sl=" << b.sl << ")"
                  << " | TR(T=" << b.tr.trds << " PF=" << std::fixed << std::setprecision(2) << b.tr.pf << " $" << std::setprecision(0) << b.tr.net
                  << ") | OOS(T=" << oo.trds << " PF=" << std::setprecision(2) << oo.pf << " $" << std::setprecision(0) << oo.net
                  << ") | Pess8y=$" << pe.net << " | NoiseMC=" << std::setprecision(1) << bm.profitable_pct << "% ShLo=" << std::setprecision(2) << bm.ci95_sharpe_low << std::endl;
    }
    return 0;
}
