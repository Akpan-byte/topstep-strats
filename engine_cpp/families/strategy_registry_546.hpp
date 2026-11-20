#pragma once
#include "../universal_11stage_plug_and_play_engine.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace s546 {

struct RegisteredStrategy {
    std::string uid;
    std::string display_name;
    std::string super_family;
    std::string source_note;
    std::string fidelity;           // EXACT_SOURCE | EXACT_FORMULA | CROSS_ASSET_CAUSAL
    bool needs_cross_asset = false;
    int32_t session_mask = 0xF;     // bit0=Asian, bit1=London, bit2=NY_AM, bit3=NY_PM
    u11::ParamSchema schema;
    u11::ParameterizedPluginFactory factory; // user_ctx = const u11::InstrumentSpec*
};

// Causal rolling ring buffer helpers usable inside any strategy plugin's state
struct RollingWindow {
    std::vector<double> buf;
    size_t head = 0;
    size_t count = 0;
    double sum = 0.0;
    double sum_sq = 0.0;

    explicit RollingWindow(size_t cap = 60) : buf(cap, 0.0) {}

    void reset(size_t cap) {
        buf.assign(cap, 0.0);
        head = 0;
        count = 0;
        sum = 0.0;
        sum_sq = 0.0;
    }

    void push(double x) {
        if (buf.empty()) return;
        if (count == buf.size()) {
            double old = buf[head];
            sum -= old;
            sum_sq -= old * old;
        } else {
            ++count;
        }
        buf[head] = x;
        sum += x;
        sum_sq += x * x;
        head = (head + 1) % buf.size();
    }

    bool full() const { return count == buf.size() && !buf.empty(); }
    double mean() const { return count > 0 ? sum / static_cast<double>(count) : 0.0; }
    double stdev() const {
        if (count < 2) return 0.0;
        double m = mean();
        double v = std::max(0.0, (sum_sq / static_cast<double>(count)) - m * m);
        return std::sqrt(v);
    }
    double max_val() const {
        if (count == 0) return 0.0;
        double mx = -1e18;
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (head + buf.size() - 1 - i) % buf.size();
            if (buf[idx] > mx) mx = buf[idx];
        }
        return mx;
    }
    double min_val() const {
        if (count == 0) return 0.0;
        double mn = 1e18;
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (head + buf.size() - 1 - i) % buf.size();
            if (buf[idx] < mn) mn = buf[idx];
        }
        return mn;
    }
    double at(size_t bars_ago) const {
        if (count == 0 || bars_ago >= count) return 0.0;
        size_t idx = (head + buf.size() - 1 - bars_ago) % buf.size();
        return buf[idx];
    }
};

// Leak-free factory builder: ctx_owner keeps Bundle alive for the lifetime of StrategyPlugin (ASan clean)
template <typename StateT>
inline u11::ParameterizedPluginFactory make_factory(
    const std::string& plugin_name,
    u11::PluginSignal (*on_bar_fn)(const u11::CausalBarView&, void*),
    void (*init_state_fn)(StateT&, const std::vector<double>&, const u11::InstrumentSpec*)
) {
    return [plugin_name, on_bar_fn, init_state_fn](
               const std::vector<double>& pvals, void* user_ctx
           ) -> u11::StrategyPlugin {
        const auto* spec = static_cast<const u11::InstrumentSpec*>(user_ctx);
        struct Bundle {
            std::vector<double> params;
            u11::InstrumentSpec inst;
            void (*init_fn)(StateT&, const std::vector<double>&, const u11::InstrumentSpec*);
        };
        auto b = std::make_shared<Bundle>(Bundle{
            pvals,
            spec ? *spec : u11::get_instrument_spec("NQ"),
            init_state_fn
        });
        auto create_fn = [](void* ctx) -> void* {
            const auto* bnd = static_cast<const Bundle*>(ctx);
            auto* st = new StateT();
            bnd->init_fn(*st, bnd->params, &bnd->inst);
            return st;
        };
        auto destroy_fn = [](void* s) {
            delete static_cast<StateT*>(s);
        };
        return u11::StrategyPlugin{
            plugin_name,
            on_bar_fn,
            create_fn,
            destroy_fn,
            const_cast<Bundle*>(b.get()),
            b
        };
    };
}

} // namespace s546
