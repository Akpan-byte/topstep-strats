#include "universal_11stage_plug_and_play_engine.hpp"
#include "families/strategy_registry_546.hpp"
#include "families/batch1_plugins.hpp"
#include "families/batch2_plugins.hpp"
#include "families/batch3_plugins.hpp"
#include "families/batch4_plugins.hpp"
#include "families/ig91_plugins.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <omp.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace u11;

inline bool in_switch_window(int16_t em, int mode) {
    const bool sw_asia   = (em >= 1080 && em <= 1215);
    const bool sw_london = (em >= 120  && em <= 255);
    const bool sw_pre    = (em >= 450  && em <  570);
    const bool sw_ny_am  = (em >= 570  && em <= 630);
    const bool sw_ny_pm  = (em >= 795  && em <= 885);

    switch (mode) {
        case 0: return true;
        case 1: return (sw_asia || sw_london || sw_pre || sw_ny_am || sw_ny_pm);
        case 2: return (sw_pre || sw_ny_am);
        case 3: return sw_ny_pm;
        case 4: return (sw_ny_am || sw_ny_pm);
        case 5: return (sw_asia || sw_london);
        default: return true;
    }
}

inline const char* switch_mode_label(int mode) {
    switch (mode) {
        case 0: return "Baseline_FullSession";
        case 1: return "All_5_Session_Switches";
        case 2: return "PreMkt+NY_Open_Switch(07:30-10:30)";
        case 3: return "NY_PM_Handoff_Switch(13:15-14:45)";
        case 4: return "NY_Open+NY_PM_Switches(09:30-10:30,13:15-14:45)";
        case 5: return "Asian+London_Switches(18:00-20:15,02:00-04:15)";
        default: return "Custom";
    }
}

struct SwitchWrappedCtx {
    StrategyPlugin inner;
    int mode = 1;
};

struct SwitchWrappedState {
    const SwitchWrappedCtx* ctx = nullptr;
    void* inner_state = nullptr;
};

inline StrategyPlugin wrap_plugin_for_switch_mode(const StrategyPlugin& base, int mode) {
    auto holder = std::make_shared<SwitchWrappedCtx>(SwitchWrappedCtx{base, mode});
    auto create_fn = [](void* uctx) -> void* {
        const auto* c = static_cast<const SwitchWrappedCtx*>(uctx);
        auto* st = new SwitchWrappedState();
        st->ctx = c;
        st->inner_state = c->inner.create_state ? c->inner.create_state(c->inner.user_ctx) : nullptr;
        return st;
    };
    auto destroy_fn = [](void* s) {
        auto* st = static_cast<SwitchWrappedState*>(s);
        if (st) {
            if (st->ctx && st->ctx->inner.destroy_state && st->inner_state) {
                st->ctx->inner.destroy_state(st->inner_state);
            }
            delete st;
        }
    };
    auto on_bar_fn = [](const CausalBarView& view, void* s) -> PluginSignal {
        auto* st = static_cast<SwitchWrappedState*>(s);
        PluginSignal sig = st->ctx->inner.on_bar(view, st->inner_state);
        if (sig.direction != 0) {
            int16_t em = view.md->meta[view.curr_b].et_min;
            if (!in_switch_window(em, st->ctx->mode)) {
                sig.direction = 0;
            }
        }
        return sig;
    };
    return StrategyPlugin{
        base.name,
        on_bar_fn,
        create_fn,
        destroy_fn,
        const_cast<SwitchWrappedCtx*>(holder.get()),
        holder
    };
}

inline PrecomputedMarketData make_premarket_unlocked_md(const PrecomputedMarketData& src) {
    PrecomputedMarketData dst = src;
    int8_t prev_s = -1;
    int32_t prev_d = -1;
    for (size_t b = 0; b < dst.n_bars; ++b) {
        auto& m = dst.meta[b];
        if (m.et_min >= 450 && m.et_min < 570) {
            m.sess_id = 2;
        }
        m.is_new_sess = (m.cme_day_idx != prev_d) || (m.sess_id != prev_s);
        prev_s = m.sess_id;
        prev_d = m.cme_day_idx;
    }
    return dst;
}

struct FastMetrics {
    int trades = 0;
    int wins = 0;
    double wr_pct = 0.0;
    double pf = 0.0;
    double net_5m = 0.0;
    double dd_5m = 0.0;
    double h1_net = 0.0;
    double h2_net = 0.0;
    int m_safe_2k = 1;
    double net_at_2k_dd = 0.0;
    double dd_at_2k_dd = 0.0;
    int m_sp500_dd = 1;
    double net_at_sp500_dd = 0.0;
    int dll_hits_5m = 0;
    std::vector<DayRecord> days;
};

FastMetrics evaluate_fast(const PrecomputedMarketData& md,
                          const PrecomputedMarketData& md_nq,
                          const PrecomputedMarketData& md_es,
                          const PrecomputedMarketData& md_ym,
                          const StrategyPlugin& pl,
                          const InstrumentSpec& sp,
                          int32_t sess_mask,
                          double sp500_dd_usd)
{
    EngineConfig cfg;
    cfg.name = pl.name;
    cfg.inst = sp;
    cfg.contracts_micros = 5;
    cfg.enforce_prop_limits_on_core = false;
    cfg.run_rolling_combines = false;
    cfg.run_full_11_stages = false;
    cfg.ref_nq = &md_nq;
    cfg.ref_es = &md_es;
    cfg.ref_ym = &md_ym;
    for (int s = 0; s < 4; ++s) cfg.allow_session[s] = ((sess_mask >> s) & 1) != 0;

    u11::detail::ChunkOutput core = u11::detail::simulate_chunk(md, pl, cfg, 0, md.total_days - 1, true);

    FastMetrics fm;
    fm.trades = static_cast<int>(core.trades.size());
    double gw = 0.0, gl = 0.0;
    for (const auto& t : core.trades) {
        if (t.net_usd > 0.0) { fm.wins++; gw += t.net_usd; }
        else gl += std::abs(t.net_usd);
        int32_t ymd = md.day_yyyymmdd[static_cast<size_t>(t.cme_day_idx)];
        if (ymd <= 20201231) fm.h1_net += t.net_usd;
        else fm.h2_net += t.net_usd;
    }

    double eq = 0.0, peak = 0.0, max_dd = 0.0;
    for (const auto& d : core.days) {
        eq += d.day_pnl;
        peak = std::max(peak, eq);
        double eod_dd = peak - eq;
        double intra_dd = peak - (eq - d.day_pnl + d.worst_intra_delta);
        max_dd = std::max({max_dd, eod_dd, intra_dd});
        if (d.day_pnl <= -1000.0 || d.worst_intra_delta <= -1000.0) fm.dll_hits_5m++;
    }

    fm.wr_pct = (fm.trades > 0) ? (100.0 * fm.wins / fm.trades) : 0.0;
    fm.pf = (gl > 1e-9) ? (gw / gl) : (gw > 0.0 ? 99.0 : 0.0);
    fm.net_5m = eq;
    fm.dd_5m = max_dd;
    fm.days = std::move(core.days);

    double dd_1m = max_dd / 5.0;
    double net_1m = eq / 5.0;
    if (dd_1m > 1.0) {
        fm.m_safe_2k = std::clamp(static_cast<int>(std::floor(1800.0 / dd_1m)), 1, 50);
        fm.m_sp500_dd = std::clamp(static_cast<int>(std::floor(sp500_dd_usd / dd_1m)), 1, 150);
    } else {
        fm.m_safe_2k = 10;
        fm.m_sp500_dd = 50;
    }
    fm.net_at_2k_dd = net_1m * fm.m_safe_2k;
    fm.dd_at_2k_dd = dd_1m * fm.m_safe_2k;
    fm.net_at_sp500_dd = net_1m * fm.m_sp500_dd;
    return fm;
}

static InstrumentSpec realistic_spec(const std::string& sym) {
    InstrumentSpec sp = get_instrument_spec(sym);
    if (sym == "NQ") { sp.entry_slip_pts = 0.50; sp.exit_slip_pts = 0.50; }
    else if (sym == "ES") { sp.entry_slip_pts = 0.25; sp.exit_slip_pts = 0.25; }
    return sp;
}

struct AuditRow {
    std::string uid;
    std::string asset;
    FastMetrics base_ny;
    FastMetrics base_all24;
    int best_sw_mode = 1;
    FastMetrics best_sw;
    FastMetrics sw_all5;
};

int main(int argc, char** argv) {
    int shard_id = 0;
    int num_shards = 1;
    std::string data_dir = "/home/akpan/topstep-strats/data";
    std::string out_csv = "session_switch_shard_0.csv";
    std::string out_days_csv = "session_switch_days_0.csv";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--shard-id" && i + 1 < argc) shard_id = std::stoi(argv[++i]);
        else if (arg == "--num-shards" && i + 1 < argc) num_shards = std::stoi(argv[++i]);
        else if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--out-csv" && i + 1 < argc) out_csv = argv[++i];
        else if (arg == "--out-days" && i + 1 < argc) out_days_csv = argv[++i];
    }

    std::vector<s546::RegisteredStrategy> reg;
    s546::register_batch1(reg);
    s546::register_batch2(reg);
    s546::register_batch3(reg);
    s546::register_batch4(reg);
    s546::register_ig91(reg);

    std::cout << "[Shard " << shard_id << "/" << num_shards << "] Loaded " << reg.size()
              << " strategies (" << (reg.size() * 3) << " pairs)." << std::endl;

    PrecomputedMarketData md_nq, md_es, md_ym;
    if (!md_nq.load_binary_auto(data_dir + "/nq_bars_10yr.bin", true, 0, CANONICAL_IS_END_YYYYMMDD) ||
        !md_es.load_binary_auto(data_dir + "/es_1min.bin", true, 0, CANONICAL_IS_END_YYYYMMDD) ||
        !md_ym.load_binary_auto(data_dir + "/ym_1min.bin", true, 0, CANONICAL_IS_END_YYYYMMDD)) {
        std::cerr << "FATAL loading market bins from " << data_dir << std::endl;
        return 1;
    }

    PrecomputedMarketData md_nq_sw = make_premarket_unlocked_md(md_nq);
    PrecomputedMarketData md_es_sw = make_premarket_unlocked_md(md_es);
    PrecomputedMarketData md_ym_sw = make_premarket_unlocked_md(md_ym);

    SP500Benchmark sp500 = compute_sp500_benchmark(md_es);
    double years = exact_calendar_years(sp500.yyyymmdd.front(), sp500.yyyymmdd.back());
    double sp500_mult = std::pow(1.0 + sp500.cagr_pct / 100.0, years);
    double sp500_net_on_50k = 50000.0 * (sp500_mult - 1.0);
    double sp500_dd_on_50k  = 50000.0 * (sp500.max_dd_pct / 100.0);

    struct AssetPair {
        const char* sym;
        const PrecomputedMarketData* md_base;
        const PrecomputedMarketData* md_sw;
    };
    const AssetPair assets[3] = {
        {"NQ", &md_nq, &md_nq_sw},
        {"ES", &md_es, &md_es_sw},
        {"YM", &md_ym, &md_ym_sw}
    };

    const size_t total_pairs = reg.size() * 3;
    std::vector<size_t> my_indices;
    for (size_t idx = static_cast<size_t>(shard_id); idx < total_pairs; idx += static_cast<size_t>(num_shards)) {
        my_indices.push_back(idx);
    }

    std::cout << "[Shard " << shard_id << "] Processing " << my_indices.size() << " pairs..." << std::endl;
    std::vector<AuditRow> rows(my_indices.size());
    int done_cnt = 0;

    #pragma omp parallel for schedule(dynamic)
    for (size_t k = 0; k < my_indices.size(); ++k) {
        size_t idx = my_indices[k];
        size_t si = idx / 3;
        size_t ai = idx % 3;
        const auto& rs = reg[si];
        const auto& ap = assets[ai];
        InstrumentSpec sp = realistic_spec(ap.sym);
        StrategyPlugin base_pl = rs.factory(rs.schema.defaults(), &sp);

        AuditRow r;
        r.uid = rs.uid;
        r.asset = ap.sym;

        r.base_ny = evaluate_fast(*ap.md_base, md_nq, md_es, md_ym, base_pl, sp, 0xC, sp500_dd_on_50k);
        r.base_all24 = evaluate_fast(*ap.md_base, md_nq, md_es, md_ym, base_pl, sp, 0xF, sp500_dd_on_50k);

        StrategyPlugin sw1_pl = wrap_plugin_for_switch_mode(base_pl, 1);
        r.sw_all5 = evaluate_fast(*ap.md_sw, md_nq_sw, md_es_sw, md_ym_sw, sw1_pl, sp, 0xF, sp500_dd_on_50k);
        r.best_sw_mode = 1;
        r.best_sw = r.sw_all5;

        auto score_fn = [](const FastMetrics& m) -> double {
            if (m.trades < 25 || m.net_5m <= 0.0) return -1e18;
            bool split_ok = (m.h1_net > 0.0 && m.h2_net > 0.0);
            return (split_ok ? 1.0 : 0.25) * m.net_at_2k_dd * std::min(m.pf, 3.0);
        };

        double best_sc = score_fn(r.best_sw);
        for (int mode = 2; mode <= 5; ++mode) {
            StrategyPlugin swm_pl = wrap_plugin_for_switch_mode(base_pl, mode);
            FastMetrics cand = evaluate_fast(*ap.md_sw, md_nq_sw, md_es_sw, md_ym_sw, swm_pl, sp, 0xF, sp500_dd_on_50k);
            double sc = score_fn(cand);
            if (sc > best_sc) {
                best_sc = sc;
                r.best_sw_mode = mode;
                r.best_sw = std::move(cand);
            }
        }
        if (r.best_sw.net_5m <= 0.0 || r.best_sw.pf < 1.15) {
            r.best_sw.days.clear();
        }
        r.base_ny.days.clear();
        r.base_all24.days.clear();
        r.sw_all5.days.clear();
        bool is_sw_winner = (r.best_sw.trades >= 25 && r.best_sw.pf >= 1.25 && r.best_sw.h1_net > 0.0 && r.best_sw.h2_net > 0.0);
        int cur_done = 0;
        #pragma omp atomic capture
        cur_done = ++done_cnt;

        if (is_sw_winner || cur_done % 10 == 0 || cur_done == static_cast<int>(my_indices.size())) {
            #pragma omp critical
            {
                std::cout << "[Shard " << shard_id << " " << cur_done << "/" << my_indices.size() << "] "
                          << r.uid << " (" << r.asset << ") | BasePF=" << std::fixed << std::setprecision(2) << r.base_ny.pf
                          << " -> SwPF=" << r.best_sw.pf << " Net5M=$" << std::setprecision(0) << r.best_sw.net_5m
                          << " DD=$" << r.best_sw.dd_5m << (is_sw_winner ? " [WINNER]" : "") << std::endl;
            }
        }
        rows[k] = std::move(r);
    }

    std::ofstream fout(out_csv);
    fout << "uid,asset,base_ny_trds,base_ny_wr,base_ny_pf,base_ny_net5m,base_ny_dd5m,base_ny_h1,base_ny_h2,"
         << "base24_trds,base24_pf,base24_net5m,base24_h1,base24_h2,"
         << "best_sw_mode,sw_trds,sw_wr,sw_pf,sw_net5m,sw_dd5m,sw_h1,sw_h2,"
         << "sw_m_safe_2k,sw_net_2k,sw_dd_2k,sw_m_sp500,sw_net_sp500,"
         << "sw_all5_trds,sw_all5_pf,sw_all5_net5m,sp500_net_50k,sp500_dd_50k\n"
         << std::fixed << std::setprecision(4);

    std::ofstream fdays(out_days_csv);
    fdays << "uid,asset,cme_day_idx,day_pnl\n" << std::fixed << std::setprecision(2);

    for (const auto& r : rows) {
        fout << r.uid << "," << r.asset << ","
             << r.base_ny.trades << "," << r.base_ny.wr_pct << "," << r.base_ny.pf << "," << r.base_ny.net_5m << "," << r.base_ny.dd_5m << "," << r.base_ny.h1_net << "," << r.base_ny.h2_net << ","
             << r.base_all24.trades << "," << r.base_all24.pf << "," << r.base_all24.net_5m << "," << r.base_all24.h1_net << "," << r.base_all24.h2_net << ","
             << r.best_sw_mode << "," << r.best_sw.trades << "," << r.best_sw.wr_pct << "," << r.best_sw.pf << "," << r.best_sw.net_5m << "," << r.best_sw.dd_5m << "," << r.best_sw.h1_net << "," << r.best_sw.h2_net << ","
             << r.best_sw.m_safe_2k << "," << r.best_sw.net_at_2k_dd << "," << r.best_sw.dd_at_2k_dd << "," << r.best_sw.m_sp500_dd << "," << r.best_sw.net_at_sp500_dd << ","
             << r.sw_all5.trades << "," << r.sw_all5.pf << "," << r.sw_all5.net_5m << "," << sp500_net_on_50k << "," << sp500_dd_on_50k << "\n";

        if (r.best_sw.trades >= 25 && r.best_sw.pf >= 1.25 && r.best_sw.h1_net > 0.0 && r.best_sw.h2_net > 0.0 && !r.best_sw.days.empty()) {
            for (const auto& d : r.best_sw.days) {
                if (std::abs(d.day_pnl) > 1e-6) {
                    fdays << r.uid << "," << r.asset << "," << d.cme_day_idx << "," << d.day_pnl << "\n";
                }
            }
        }
    }
    fout.close();
    fdays.close();
    std::cout << "[Shard " << shard_id << "] Done! Wrote " << out_csv << std::endl;
    return 0;
}
