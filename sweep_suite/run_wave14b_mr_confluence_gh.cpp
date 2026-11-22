#include "wave7_zero_bug_engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace w7;

inline std::vector<CausalEntryEvent> extract_mr14_param(
    double vwap_z_th, double bb_z_th, double sl_mult,
    int8_t target_sess, int8_t sym_id,
    const PrecomputedMarketData& md, const MasterTimeline& tl, const InstrumentSpec& inst)
{
    std::vector<CausalEntryEvent> out;
    const size_t N = md.n_bars;
    if (N < 500) return out;
    const double tick = inst.tick_size;

    int64_t cb5 = -1;
    double c5_o = md.bars[0].o, c5_h = md.bars[0].h, c5_l = md.bars[0].l, c5_c = md.bars[0].c;
    double prev5_c = std::max(tick, static_cast<double>(md.bars[0].c)), cur_atr5 = tick * 8.0;
    std::deque<double> tr14, c5_hist, vw_dist_hist;
    int32_t last_entry_b = -100;

    for (size_t b = 0; b + 1 < N; ++b) {
        const auto& rb = md.bars[b]; const auto& m = md.meta[b];
        double bo = std::max(tick, static_cast<double>(rb.o));
        double bh = std::max(bo, static_cast<double>(rb.h));
        double bl = std::min(bo, std::max(tick, static_cast<double>(rb.l)));
        double bc = std::max(tick, static_cast<double>(rb.c));

        int64_t b5_id = (static_cast<int64_t>(m.cme_day_idx) * 1440LL + static_cast<int64_t>(m.et_min)) / 5LL;
        if (b5_id != cb5) { cb5 = b5_id; c5_o = bo; c5_h = bh; c5_l = bl; c5_c = bc; }
        else { c5_h = std::max(c5_h, bh); c5_l = std::min(c5_l, bl); c5_c = bc; }

        const auto& m_next = md.meta[b + 1];
        int64_t next_b5 = (static_cast<int64_t>(m_next.cme_day_idx) * 1440LL + static_cast<int64_t>(m_next.et_min)) / 5LL;
        if (next_b5 == cb5) continue;

        double tr = std::max({c5_h - c5_l, std::abs(c5_h - prev5_c), std::abs(c5_l - prev5_c)});
        tr14.push_back(tr); if (tr14.size() > 14) tr14.pop_front();
        double s_tr = 0.0; for (double x : tr14) s_tr += x;
        cur_atr5 = std::max(tick * 2.0, s_tr / static_cast<double>(tr14.size()));
        prev5_c = c5_c;

        c5_hist.push_back(c5_c); if (c5_hist.size() > 30) c5_hist.pop_front();
        double vwap = (md.session_vwap[b] > tick) ? md.session_vwap[b] : c5_c;
        vw_dist_hist.push_back(c5_c - vwap); if (vw_dist_hist.size() > 30) vw_dist_hist.pop_front();

        if (c5_hist.size() < 20 || vw_dist_hist.size() < 20) continue;

        // BB(20) z-score
        double m_bb = 0; for (size_t i = c5_hist.size() - 20; i < c5_hist.size(); ++i) m_bb += c5_hist[i];
        m_bb /= 20.0;
        double v_bb = 0; for (size_t i = c5_hist.size() - 20; i < c5_hist.size(); ++i) { double d = c5_hist[i] - m_bb; v_bb += d * d; }
        double sd_bb = std::max(tick, std::sqrt(v_bb / 20.0));
        double bb_z = (c5_c - m_bb) / sd_bb;

        // VWAP std z-score
        double m_vw = 0; for (size_t i = vw_dist_hist.size() - 20; i < vw_dist_hist.size(); ++i) m_vw += vw_dist_hist[i];
        m_vw /= 20.0;
        double v_vw = 0; for (size_t i = vw_dist_hist.size() - 20; i < vw_dist_hist.size(); ++i) { double d = vw_dist_hist[i] - m_vw; v_vw += d * d; }
        double sd_vw = std::max(tick, std::sqrt(v_vw / 20.0));
        double vw_z = (c5_c - vwap) / sd_vw;

        // ER12 chop filter
        double net12 = std::abs(c5_hist.back() - c5_hist[c5_hist.size() - 13]);
        double path12 = 0; for (size_t i = c5_hist.size() - 12; i < c5_hist.size(); ++i) path12 += std::abs(c5_hist[i] - c5_hist[i - 1]);
        double er12 = (path12 > 1e-9) ? (net12 / path12) : 0.0;

        int8_t dir = 0;
        if (er12 <= 0.55) {
            if (vw_z <= -vwap_z_th && bb_z <= -bb_z_th && c5_c > c5_o) dir = 1;
            else if (vw_z >= vwap_z_th && bb_z >= bb_z_th && c5_c < c5_o) dir = -1;
        }
        if (dir == 0 || m_next.cme_day_idx != m.cme_day_idx || m_next.sess_id != target_sess) continue;
        int32_t ymd = md.day_yyyymmdd[static_cast<size_t>(m_next.cme_day_idx)];
        if (ymd < 20180101 || ymd > CANONICAL_IS_END_YYYYMMDD) continue;
        int32_t tl_i = tl.b_to_tl[sym_id][b + 1];
        if (tl_i < 0 || static_cast<int32_t>(b) - last_entry_b < 5) continue;
        last_entry_b = static_cast<int32_t>(b);

        CausalEntryEvent ev{};
        ev.ymd = ymd; ev.year = ymd / 10000; ev.entry_tl = static_cast<size_t>(tl_i);
        ev.sess_id = m_next.sess_id; ev.sym_id = sym_id; ev.dir = dir;
        ev.leg_id = 800; ev.fam_id = 8; ev.is_tier_s = true;
        ev.sl_pts = std::max(tick * 6.0, sl_mult * cur_atr5);
        ev.is_mr_trail = true; ev.mr_atr5_entry = cur_atr5;
        ev.mr_tgt_px = vwap;
        ev.tp_pts = 2.5 * ev.sl_pts; ev.max_hold_bars = 120;
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

    std::cout << "=== WAVE 14B (GH ACTIONS SHARD " << cell_id << ": " << id_to_sym(sym) << " | " << sess_name(sess) << ") ===" << std::endl;
    struct GridPt { double vz, bz, sl; PortMetrics5C h1, h2, tr; };
    std::vector<GridPt> pts;
    for (double vz : {1.2, 1.6, 2.0})
        for (double bz : {1.5, 2.0, 2.5})
            for (double sl : {1.2, 1.5, 2.0})
                pts.push_back({vz, bz, sl, {}, {}, {}});

    #pragma omp parallel for schedule(dynamic, 1)
    for (size_t i = 0; i < pts.size(); ++i) {
        auto evs = extract_mr14_param(pts[i].vz, pts[i].bz, pts[i].sl, sess, sym, *mds[sym], tl, insts[sym]);
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
    std::cout << "  TRAIN Plateau Pass=" << pass_cnt << "/27 (" << (pass_cnt * 100 / 27) << "%)" << std::endl;
    if (best_i >= 0) {
        auto& b = pts[best_i];
        auto evs = extract_mr14_param(b.vz, b.bz, b.sl, sess, sym, *mds[sym], tl, insts[sym]);
        auto oo = summarize_sim_metrics("OO", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20220101, 20251231, 200.0, false, false), 9);
        auto pe = summarize_sim_metrics("PE", run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, pess_insts, 20180101, 20251231, 200.0, false, false), 9);
        EngineConfig cfg{}; double nsh = 0.0;
        auto sr8 = run_simultaneous_portfolio_sim(tl, no_htf, mr_5m_ptrs, evs, insts, 20180101, 20251231, 200.0, false, false);
        auto bm = run_multi_asset_noise_mc_30k(sr8.u11_trades, sr8.u11_trade_syms, static_cast<int32_t>(sr8.days.size()), insts, cfg, 0.0, &nsh);
        std::cout << "  BEST(" << b.vz << "," << b.bz << "," << b.sl << ") | TR(T=" << b.tr.trds << " PF=" << std::fixed << std::setprecision(2) << b.tr.pf << " $" << std::setprecision(0) << b.tr.net
                  << ") | OOS(T=" << oo.trds << " PF=" << std::setprecision(2) << oo.pf << " $" << std::setprecision(0) << oo.net
                  << ") | Pess8y=$" << pe.net << " | NoiseMC=" << std::setprecision(1) << bm.profitable_pct << "% ShLo=" << std::setprecision(2) << bm.ci95_sharpe_low << std::endl;
    }
    return 0;
}
