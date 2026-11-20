#include "universal_11stage_plug_and_play_engine.hpp"
#include "families/strategy_registry_546.hpp"
#include "families/batch1_plugins.hpp"
#include "families/batch2_plugins.hpp"
#include "families/batch3_plugins.hpp"
#include "families/batch4_plugins.hpp"
#include "families/ig91_plugins.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace u11;

struct SwitchCandidate {
    std::string uid;
    std::string asset;
    int32_t sess_mask;
    int switch_mode;
};

inline bool in_switch_window(int16_t em, int mode) {
    const bool sw_asia   = (em >= 1080 && em <= 1215);
    const bool sw_london = (em >= 120  && em <= 255);
    const bool sw_pre    = (em >= 450  && em <  570);
    const bool sw_ny_am  = (em >= 570  && em <= 630);
    const bool sw_ny_pm  = (em >= 795  && em <= 885);
    switch (mode) {
        case 1: return (sw_asia || sw_london || sw_pre || sw_ny_am || sw_ny_pm);
        case 2: return (sw_pre || sw_ny_am);
        case 3: return sw_ny_pm;
        case 4: return (sw_ny_am || sw_ny_pm);
        case 5: return (sw_asia || sw_london);
        default: return true;
    }
}

struct SwitchCtx { StrategyPlugin inner; int mode; };
struct SwitchState { const SwitchCtx* ctx; void* inner_state; };

inline StrategyPlugin wrap_switch(const StrategyPlugin& base, int mode) {
    auto holder = std::make_shared<SwitchCtx>(SwitchCtx{base, mode});
    auto create_fn = [](void* uctx) -> void* {
        const auto* c = static_cast<const SwitchCtx*>(uctx);
        auto* st = new SwitchState{c, c->inner.create_state ? c->inner.create_state(c->inner.user_ctx) : nullptr};
        return st;
    };
    auto destroy_fn = [](void* s) {
        auto* st = static_cast<SwitchState*>(s);
        if (st) {
            if (st->ctx && st->ctx->inner.destroy_state && st->inner_state) st->ctx->inner.destroy_state(st->inner_state);
            delete st;
        }
    };
    auto on_bar_fn = [](const CausalBarView& view, void* s) -> PluginSignal {
        auto* st = static_cast<SwitchState*>(s);
        PluginSignal sig = st->ctx->inner.on_bar(view, st->inner_state);
        if (sig.direction != 0 && !in_switch_window(view.md->meta[view.curr_b].et_min, st->ctx->mode)) {
            sig.direction = 0;
        }
        return sig;
    };
    return StrategyPlugin{base.name, on_bar_fn, create_fn, destroy_fn, const_cast<SwitchCtx*>(holder.get()), holder};
}

static InstrumentSpec realistic_spec(const std::string& sym) {
    InstrumentSpec sp = get_instrument_spec(sym);
    if (sym == "NQ") { sp.entry_slip_pts = 0.50; sp.exit_slip_pts = 0.50; }
    else if (sym == "ES") { sp.entry_slip_pts = 0.25; sp.exit_slip_pts = 0.25; }
    return sp;
}

static PrecomputedMarketData unlock_premarket(const PrecomputedMarketData& src) {
    PrecomputedMarketData dst = src;
    int8_t prev_s = -1; int32_t prev_d = -1;
    for (size_t b = 0; b < dst.n_bars; ++b) {
        if (dst.meta[b].et_min >= 450 && dst.meta[b].et_min < 570) dst.meta[b].sess_id = 2;
        dst.meta[b].is_new_sess = (dst.meta[b].cme_day_idx != prev_d) || (dst.meta[b].sess_id != prev_s);
        prev_s = dst.meta[b].sess_id; prev_d = dst.meta[b].cme_day_idx;
    }
    return dst;
}

int main(int argc, char** argv) {
    int shard_id = 0, num_shards = 1;
    std::string data_dir = "/tmp/market_data";
    std::string out_csv = "/tmp/out/cert_shard_0.csv";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shard-id" && i + 1 < argc) shard_id = std::stoi(argv[++i]);
        else if (a == "--num-shards" && i + 1 < argc) num_shards = std::stoi(argv[++i]);
        else if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--out-csv" && i + 1 < argc) out_csv = argv[++i];
    }

    std::vector<s546::RegisteredStrategy> reg;
    s546::register_batch1(reg); s546::register_batch2(reg); s546::register_batch3(reg);
    s546::register_batch4(reg); s546::register_ig91(reg);
    std::unordered_map<std::string, size_t> uid_map;
    for (size_t i = 0; i < reg.size(); ++i) uid_map[reg[i].uid] = i;

    std::vector<SwitchCandidate> cands = {
        {"reel46_16_single_candle_rejection_hammer_shooting_star", "ES", 0x4, 2},
        {"phys_151", "NQ", 0x3, 5},
        {"chat_arch_233_van_der_pol_rupture", "NQ", 0xC, 4},
        {"phys_013", "ES", 0x3, 5},
        {"kernel_reel_5", "NQ", 0xC, 4},
        {"orb_cooptimized_champions", "NQ", 0x3, 5},
        {"phys_173", "NQ", 0x8, 3},
        {"phys_170", "NQ", 0x8, 3},
        {"phys_174", "NQ", 0x8, 3},
        {"topstep_paper1_ict_silver_bullet", "ES", 0xC, 4},
        {"chat_arch_210_block_momentum_ignition", "NQ", 0xC, 4},
        {"phys_157", "YM", 0x3, 5},
        {"topstep_asian_paper_tradeciety_bollinger_rsi", "ES", 0xF, 1},
        {"phys_001", "YM", 0x8, 3},
        {"phys_001", "ES", 0x8, 3},
        {"insta_batch2_session_liquidity", "NQ", 0x8, 3},
        {"phys_001", "NQ", 0x8, 3},
        {"topstep_quant_suite12_climax_snapback", "ES", 0xF, 1},
        {"kernel_reel_6", "YM", 0xC, 4},
        {"ig91_063_williams_r_ema_vwap", "YM", 0xF, 1}
    };

    PrecomputedMarketData md_nq_raw, md_es_raw, md_ym_raw;
    if (!md_nq_raw.load_binary_auto(data_dir + "/nq_bars_10yr.bin", true, 0, CANONICAL_IS_END_YYYYMMDD) ||
        !md_es_raw.load_binary_auto(data_dir + "/es_1min.bin", true, 0, CANONICAL_IS_END_YYYYMMDD) ||
        !md_ym_raw.load_binary_auto(data_dir + "/ym_1min.bin", true, 0, CANONICAL_IS_END_YYYYMMDD)) {
        return 1;
    }
    PrecomputedMarketData md_nq = unlock_premarket(md_nq_raw);
    PrecomputedMarketData md_es = unlock_premarket(md_es_raw);
    PrecomputedMarketData md_ym = unlock_premarket(md_ym_raw);
    SP500Benchmark sp500 = compute_sp500_benchmark(md_es);

    std::ofstream fout(out_csv);
    fout << "uid,asset,sess_mask,switch_mode,trades,wr_pct,pf,net_5m,dd_5m,m_safe,net_msafe,dd_msafe,"
         << "pass_pct,med_ttp,be_slip_pts,tier,best_params\n" << std::fixed << std::setprecision(4);

    for (size_t i = static_cast<size_t>(shard_id); i < cands.size(); i += static_cast<size_t>(num_shards)) {
        const auto& c = cands[i];
        auto it = uid_map.find(c.uid);
        if (it == uid_map.end()) continue;
        const auto& rs = reg[it->second];
        const PrecomputedMarketData& md = (c.asset == "NQ") ? md_nq : ((c.asset == "ES") ? md_es : md_ym);
        InstrumentSpec sp = realistic_spec(c.asset);

        // Stage 5 Parameter Sweep around defaults (up to 16 random/grid trials) per (Strategy, Index, Session)
        auto grid = rs.schema.sample_latin_hypercube(16, 20261120ULL + i);
        grid.insert(grid.begin(), rs.schema.defaults());

        std::vector<double> best_params = rs.schema.defaults();
        double best_score = -1e18;
        Full11StageResult best_res;
        bool has_best = false;

        for (const auto& pv : grid) {
            StrategyPlugin pl = wrap_switch(rs.factory(pv, &sp), c.switch_mode);
            EngineConfig cfg;
            cfg.name = c.uid + "_" + c.asset;
            cfg.inst = sp;
            cfg.contracts_micros = 5;
            cfg.enforce_prop_limits_on_core = false;
            cfg.run_rolling_combines = false;
            cfg.run_full_11_stages = false;
            cfg.ref_nq = &md_nq; cfg.ref_es = &md_es; cfg.ref_ym = &md_ym;
            for (int s = 0; s < 4; ++s) cfg.allow_session[s] = ((c.sess_mask >> s) & 1) != 0;

            auto core = run_chunked_parallel_11stage(md, pl, cfg);
            double sc = (core.h1.net_pnl > 0 && core.h2.net_pnl > 0) ? (core.net_pnl_5m * std::min(core.profit_factor, 3.0)) : -1e12;
            if (!has_best || sc > best_score) {
                best_score = sc;
                best_params = pv;
                has_best = true;
            }
        }

        // Full 11-Stage Certification on Best Tuned Params
        StrategyPlugin best_pl = wrap_switch(rs.factory(best_params, &sp), c.switch_mode);
        EngineConfig cfg;
        cfg.name = c.uid + "_" + c.asset + "_SW" + std::to_string(c.switch_mode);
        cfg.inst = sp;
        cfg.contracts_micros = 5;
        cfg.dsr_num_trials = static_cast<int>(grid.size());
        cfg.run_rolling_combines = true;
        cfg.run_full_11_stages = true;
        cfg.ref_nq = &md_nq; cfg.ref_es = &md_es; cfg.ref_ym = &md_ym;
        for (int s = 0; s < 4; ++s) cfg.allow_session[s] = ((c.sess_mask >> s) & 1) != 0;

        best_res = run_chunked_parallel_11stage(md, best_pl, cfg, &sp500);

        std::string pstr;
        for (size_t k = 0; k < best_params.size(); ++k) {
            if (k) pstr += ";";
            pstr += std::to_string(best_params[k]);
        }

        std::cout << "[Cert " << i << "] " << c.uid << " (" << c.asset << ") -> "
                  << best_res.inst_class.tier << " | PF=" << best_res.profit_factor
                  << " | M_safe=" << best_res.recommended_m_safe
                  << " | Pass=" << best_res.combine_m_safe.pass_rate_pct << "%\n" << std::flush;

        fout << c.uid << "," << c.asset << "," << c.sess_mask << "," << c.switch_mode << ","
             << best_res.total_trades << "," << best_res.win_rate_pct << "," << best_res.profit_factor << ","
             << best_res.net_pnl_5m << "," << best_res.curve_max_dd_5m << ","
             << best_res.recommended_m_safe << "," << best_res.net_pnl_m_safe << "," << best_res.curve_max_dd_m_safe << ","
             << best_res.combine_m_safe.pass_rate_pct << "," << best_res.combine_m_safe.ttp.p50 << ","
             << best_res.inst_class.breakeven_slip_pts << "," << best_res.inst_class.tier << ",\"" << pstr << "\"\n";
    }
    return 0;
}
