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

// Pure Indicator-Free Causal Price-Action Intuition Engine:
// Discretizes raw 5m candle geometry (body sign, range vs rolling median, position in 6-bar swing)
// and learns online causal expected R-outcome per state ONLY after each prior horizon completes.
inline std::vector<CausalEntryEvent> extract_intuition_cell(
    int horizon_5m, double decay, int min_exp, double conf_th, double sl_mult,
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
    std::deque<double> tr14, rng12, hi6, lo6;

    // 54 raw price-action states: body(3) x range_regime(3) x swing_loc(3) x prev_body(2)
    static constexpr int N_STATES = 54;
    std::array<double, N_STATES> exp_score{};
    std::array<double, N_STATES> exp_cnt{};
    struct PendingObs { size_t mature_5m_idx; int state; double entry_px; double risk_px; };
    std::deque<PendingObs> pending;

    size_t bar5_idx = 0;
    int prev_body_sign = 0;
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

        ++bar5_idx;
        double rng = std::max(tick, c5_h - c5_l);
        double tr = std::max({rng, std::abs(c5_h - prev5_c), std::abs(c5_l - prev5_c)});
        tr14.push_back(tr); if (tr14.size() > 14) tr14.pop_front();
        double s_tr = 0.0; for (double x : tr14) s_tr += x;
        cur_atr5 = std::max(tick * 2.0, s_tr / static_cast<double>(tr14.size()));
        prev5_c = c5_c;

        rng12.push_back(rng); if (rng12.size() > 12) rng12.pop_front();
        hi6.push_back(c5_h); if (hi6.size() > 6) hi6.pop_front();
        lo6.push_back(c5_l); if (lo6.size() > 6) lo6.pop_front();

        // Strictly causal update: mature ONLY observations whose horizon completed <= now
        while (!pending.empty() && pending.front().mature_5m_idx <= bar5_idx) {
            auto obs = pending.front(); pending.pop_front();
            double r_ret = std::clamp((c5_c - obs.entry_px) / std::max(tick, obs.risk_px), -3.0, 3.0);
            exp_score[obs.state] = decay * exp_score[obs.state] + (1.0 - decay) * r_ret;
            exp_cnt[obs.state] = decay * exp_cnt[obs.state] + 1.0;
        }

        if (rng12.size() < 12 || hi6.size() < 6) {
            prev_body_sign = (c5_c >= c5_o) ? 1 : 0;
            continue;
        }

        // Raw candle state discretization (zero indicators)
        double body = c5_c - c5_o;
        int b_bin = (std::abs(body) < 0.25 * rng) ? 1 : ((body > 0.0) ? 2 : 0);
        double avg_rng = 0.0; for (double x : rng12) avg_rng += x; avg_rng /= 12.0;
        int r_bin = (rng < 0.75 * avg_rng) ? 0 : ((rng > 1.25 * avg_rng) ? 2 : 1);
        double hh = hi6[0], ll = lo6[0];
        for (size_t i = 1; i < 6; ++i) { hh = std::max(hh, hi6[i]); ll = std::min(ll, lo6[i]); }
        double loc = (c5_c - ll) / std::max(tick, hh - ll);
        int l_bin = (loc < 0.33) ? 0 : ((loc > 0.67) ? 2 : 1);
        int st = ((b_bin * 3 + r_bin) * 3 + l_bin) * 2 + prev_body_sign;
        prev_body_sign = (c5_c >= c5_o) ? 1 : 0;

        // Record new causal observation for future maturation
        pending.push_back({bar5_idx + static_cast<size_t>(horizon_5m), st, c5_c, std::max(tick * 6.0, sl_mult * cur_atr5)});

        // Act ONLY if accumulated causal experience in this state clears min_exp & conf_th
        int8_t dir = 0;
        if (exp_cnt[st] >= static_cast<double>(min_exp)) {
            if (exp_score[st] >= conf_th) dir = 1;
            else if (exp_score[st] <= -conf_th) dir = -1;
        }

        if (dir == 0 || m_next.cme_day_idx != m.cme_day_idx || m_next.sess_id != target_sess) continue;
        int32_t ymd = md.day_yyyymmdd[static_cast<size_t>(m_next.cme_day_idx)];
        if (ymd < 20180101 || ymd > CANONICAL_IS_END_YYYYMMDD) continue;
        int32_t tl_i = tl.b_to_tl[sym_id][b + 1];
        if (tl_i < 0 || static_cast<int32_t>(b) - last_entry_b < 8) continue;
        last_entry_b = static_cast<int32_t>(b);

        CausalEntryEvent ev{};
        ev.ymd = ymd; ev.year = ymd / 10000; ev.entry_tl = static_cast<size_t>(tl_i);
        ev.sess_id = m_next.sess_id; ev.sym_id = sym_id; ev.dir = dir;
        ev.leg_id = 980; ev.fam_id = 17; ev.is_tier_s = true;
        ev.sl_pts = std::max(tick * 6.0, sl_mult * cur_atr5);
        ev.two_leg_split = true; ev.is_stack_htf60 = true;
        ev.tp1_pts = 1.5 * ev.sl_pts; ev.tp_pts = 6.0 * ev.sl_pts; ev.max_hold_bars = 360;
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
    std::array<const HtfCausalLevels*, 3> no_htf = {nullptr, nullptr, nullptr};
    std::array<const std::vector<bool>*, 3> no_mr = {nullptr, nullptr, nullptr};

    std::cout << "=== WAVE 17 INDICATOR-FREE INTUITION (SHARD " << cell_id << ": " << id_to_sym(sym) << " | " << sess_name(sess) << ") ===" << std::endl;

    struct GridPt { int hor; double dec; int mexp; double conf, sl; PortMetrics5C h1, h2, tr; };
    std::vector<GridPt> pts;
    for (int hor : {6, 12})
        for (double dec : {0.94, 0.97})
            for (int mexp : {8, 15})
                for (double conf : {0.15, 0.25})
                    for (double sl : {1.2, 1.6})
                        pts.push_back({hor, dec, mexp, conf, sl, {}, {}, {}});

    #pragma omp parallel for schedule(dynamic, 1)
    for (size_t i = 0; i < pts.size(); ++i) {
        auto evs = extract_intuition_cell(pts[i].hor, pts[i].dec, pts[i].mexp, pts[i].conf, pts[i].sl, sess, sym, *mds[sym], tl, insts[sym]);
        pts[i].h1 = summarize_sim_metrics("H1", run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20180101, 20191231, 200.0, false, false), 9);
        pts[i].h2 = summarize_sim_metrics("H2", run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20200101, 20211231, 200.0, false, false), 9);
        pts[i].tr = summarize_sim_metrics("TR", run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20180101, 20211231, 200.0, false, false), 9);
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
        auto evs = extract_intuition_cell(b.hor, b.dec, b.mexp, b.conf, b.sl, sess, sym, *mds[sym], tl, insts[sym]);
        auto oo = summarize_sim_metrics("OO", run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20220101, 20251231, 200.0, false, false), 9);
        auto pe = summarize_sim_metrics("PE", run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, pess_insts, 20180101, 20251231, 200.0, false, false), 9);
        EngineConfig cfg{}; double nsh = 0.0;
        auto sr8 = run_simultaneous_portfolio_sim(tl, no_htf, no_mr, evs, insts, 20180101, 20251231, 200.0, false, false);
        auto bm = run_multi_asset_noise_mc_30k(sr8.u11_trades, sr8.u11_trade_syms, static_cast<int32_t>(sr8.days.size()), insts, cfg, 0.0, &nsh);
        std::cout << "  BEST(h=" << b.hor << ",d=" << b.dec << ",m=" << b.mexp << ",c=" << b.conf << ",sl=" << b.sl << ")"
                  << " | TR(T=" << b.tr.trds << " PF=" << std::fixed << std::setprecision(2) << b.tr.pf << " $" << std::setprecision(0) << b.tr.net
                  << ") | OOS(T=" << oo.trds << " PF=" << std::setprecision(2) << oo.pf << " $" << std::setprecision(0) << oo.net
                  << ") | Pess8y=$" << pe.net << " | NoiseMC=" << std::setprecision(1) << bm.profitable_pct << "% ShLo=" << std::setprecision(2) << bm.ci95_sharpe_low << std::endl;
    }
    return 0;
}
