#pragma once
// ============================================================================
// BATCH 2 PLUGINS: 02_Physics_Quant_Multiverse_001_240 (120 Strategies)
// ============================================================================
// Faithful, strictly causal C++17 translation of all 120 physics/quant
// strategies in batch2.json into u11::StrategyPlugin + ParamSchema.
//
// Invariants:
//   - Strict causality: ONLY CausalBarView (lag <= 0), zero lookahead.
//   - Symbol-agnostic scaling: InstrumentSpec (baseline_index, tick_size).
//   - Cross-asset causal references via view.has_ref_*() / view.ref_*_bar(lag).
//   - Every formula's lookback, threshold, SL, TP, max_hold, and ratchet
//     constants parameterized in u11::ParamSchema for Stage 5 sweeps.
// ============================================================================

#include "strategy_registry_546.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace s546 {
namespace batch2_detail {

constexpr double kPi = 3.14159265358979323846;

struct Batch2Meta {
    int id;
    const char* uid;
    const char* display_name;
    const char* source_note;
    const char* fidelity;
    bool needs_cross_asset;
    int32_t session_mask;
};

inline constexpr Batch2Meta kBatch2Meta[120] = {
    {0, "phys_001", "#001 1_CrossAsset_Imbalance", "Cross-Asset Lead-Lag & Beta Volatility Skew | multiverse_20strats_engine.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {1, "phys_002", "#002 2_Settlement_MOC", "Structural Auction Market Theory & Settlement Pinning | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {2, "phys_003", "#003 3_Overnight_Inventory_Flush", "Structural Auction Market Theory & Settlement Pinning | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {3, "phys_004", "#004 4_Fractal_Hurst_Switcher", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {4, "phys_005", "#005 5_MultiIndex_Compression", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {5, "phys_006", "#006 6_London_Fix_Expansion", "Momentum / Dynamic Range Expansion & Breakout | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {6, "phys_007", "#007 7_Volume_Absorption_Exhaustion", "Order-Flow & Volume Absorption / Liquidity Sweeps | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {7, "phys_008", "#008 8_PostShock_Deceleration", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {8, "phys_009", "#009 9_0DTE_Gamma_Pinning", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {9, "phys_010", "#010 10_Statistical_ZScore_MAD", "Mean Reversion & Statistical Arbitrage / Boundary Decay | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {10, "phys_011", "#011 11_Liquidity_Sweep_Reclaim", "Order-Flow & Volume Absorption / Liquidity Sweeps | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {11, "phys_012", "#012 12_MAD_Dynamic_Reversion", "Mean Reversion & Statistical Arbitrage / Boundary Decay | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {12, "phys_013", "#013 13_Index_Velocity_Divergence", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {13, "phys_014", "#014 14_Volume_Weighted_Entropy", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {14, "phys_015", "#015 15_Volatility_Imbalance_Shock", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {15, "phys_016", "#016 16_TrueRange_Compression_Trap", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {16, "phys_017", "#017 17_Ornstein_Uhlenbeck_Speed", "Mean Reversion & Statistical Arbitrage / Boundary Decay | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {17, "phys_018", "#018 18_Kinetic_Energy_Conservation", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {18, "phys_019", "#019 19_POC_Migration_Drift", "Mathematical Physics & Non-Linear Dynamics | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {19, "phys_020", "#020 20_Asymmetric_Wick_Absorption", "Order-Flow & Volume Absorption / Liquidity Sweeps | multiverse_20strats_engine.cpp", "EXACT_SOURCE", false, 0xF},
    {20, "phys_021", "#021 21_Hilbert_Instantaneous_Phase", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {21, "phys_022", "#022 22_Kolmogorov_Sinai_Entropy", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {22, "phys_023", "#023 23_Fisher_Information_Metric", "Mathematical Physics & Non-Linear Dynamics | 23_Fisher_Information_Metric_core.cpp", "EXACT_SOURCE", false, 0xF},
    {23, "phys_024", "#024 24_Kalman_Filter_Optimal_Drift", "Mathematical Physics & Non-Linear Dynamics | 24_Kalman_Filter_Optimal_Drift_core.cpp", "EXACT_SOURCE", false, 0xF},
    {24, "phys_025", "#025 25_Orderflow_Liquidity_Permeability", "Order-Flow & Volume Absorption / Liquidity Sweeps | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {25, "phys_026", "#026 26_Wavelet_Multiresolution_Energy", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {26, "phys_027", "#027 27_CrossAsset_Triangular_LeadLag", "Cross-Asset Lead-Lag & Beta Volatility Skew | multiverse_batch2_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {27, "phys_028", "#028 28_Hawkes_SelfExciting_Arrivals", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {28, "phys_029", "#029 29_Asymmetric_Jump_Diffusion", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {29, "phys_030", "#030 30_Dynamic_Volume_Sync_VBP", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {30, "phys_031", "#031 31_Markov_Regime_Transition_Matrix", "Mathematical Physics & Non-Linear Dynamics | 31_Markov_Regime_Transition_Matrix_core.cpp", "EXACT_SOURCE", false, 0xF},
    {31, "phys_032", "#032 32_GARCH_Volatility_Clustering", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {32, "phys_033", "#033 33_Stochastic_Resonance_Pulse", "Mean Reversion & Statistical Arbitrage / Boundary Decay | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {33, "phys_034", "#034 34_Topological_Persistence_Homology", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {34, "phys_035", "#035 35_Information_Geometric_Curvature", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {35, "phys_036", "#036 36_Microstructure_BidAsk_Reflectance", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {36, "phys_037", "#037 37_Multifractal_DFA_Spectrum", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 37_Multifractal_DFA_Spectrum_core.cpp", "EXACT_SOURCE", false, 0xF},
    {37, "phys_038", "#038 38_Lyapunov_Exponent_Divergence", "Momentum / Dynamic Range Expansion & Breakout | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {38, "phys_039", "#039 39_Synthetic_Portfolio_Cointegration", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {39, "phys_040", "#040 40_Nonlinear_Volterra_Kernel", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch2_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {40, "phys_041b_quantum_tunneling_nq_allsess", "41_Quantum_Tunneling_NQ_AllSess", "Physics_Quant_100_240 (Non-Linear Physics) | 41_Quantum_Tunneling_NQ_AllSess_core.cpp", "EXACT_FORMULA", false, 0xF},
    {41, "phys_041", "#041 41_Quantum_Tunneling_Barrier", "Mathematical Physics & Non-Linear Dynamics | 41_Quantum_Tunneling_Barrier_core.cpp", "EXACT_SOURCE", false, 0xF},
    {42, "phys_042", "#042 42_Langevin_Stochastic_Drift", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {43, "phys_043", "#043 43_EMD_Hilbert_Huang_Frequency", "Mathematical Physics & Non-Linear Dynamics | 43_EMD_Hilbert_Huang_Frequency_core.cpp", "EXACT_SOURCE", false, 0xF},
    {44, "phys_044", "#044 44_Renyi_Entropy_OrderQ_Spectrum", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 44_Renyi_Entropy_OrderQ_Spectrum_core.cpp", "EXACT_SOURCE", false, 0xF},
    {45, "phys_045", "#045 45_Fokker_Planck_Probability_Flux", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {46, "phys_046", "#046 46_Betti_Number_Zero_Persistence", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 46_Betti_Number_Zero_Persistence_core.cpp", "EXACT_SOURCE", false, 0xF},
    {47, "phys_047", "#047 47_Hawkes_Modulated_Jump_Drift", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {48, "phys_048", "#048 48_Kuramoto_Phase_Synchronization", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {49, "phys_049", "#049 49_Thermodynamic_Landauer_Work", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {50, "phys_050", "#050 50_Fractional_Brownian_Hurst_Field", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {51, "phys_051", "#051 51_Koopman_DMD_Leading_Mode", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {52, "phys_052", "#052 52_Tsallis_NonExtensive_Tail", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {53, "phys_053", "#053 53_Lie_Algebra_Hyperbolic_Shear", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {54, "phys_054", "#054 54_Graph_Laplacian_Fiedler_Cut", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {55, "phys_055", "#055 55_CWT_Morlet_Ridge_Tracking", "Mathematical Physics & Non-Linear Dynamics | 55_CWT_Morlet_Ridge_Tracking_core.cpp", "EXACT_SOURCE", false, 0xF},
    {56, "phys_056", "#056 56_Wasserstein_EarthMover_Flux", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {57, "phys_057", "#057 57_Volterra_Memory_Dissipation", "Mathematical Physics & Non-Linear Dynamics | 57_Volterra_Memory_Dissipation_core.cpp", "EXACT_SOURCE", false, 0xF},
    {58, "phys_058", "#058 58_Poincare_Map_Tangent_Divergence", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {59, "phys_059", "#059 59_Girsanov_Measure_Change_Shift", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {60, "phys_060", "#060 60_Quantum_Harmonic_Displacement", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch3_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {61, "phys_061", "#061 61_Relativistic_Lorentz_Flow", "Mathematical Physics & Non-Linear Dynamics | 61_Relativistic_Lorentz_Flow_core.cpp", "EXACT_SOURCE", false, 0xF},
    {62, "phys_062", "#062 62_SpinGlass_Frustration_Relaxation", "Mathematical Physics & Non-Linear Dynamics | 62_SpinGlass_Frustration_Relaxation_core.cpp", "EXACT_SOURCE", false, 0xF},
    {63, "phys_063", "#063 63_Burgers_Shockwave_Singularity", "Mathematical Physics & Non-Linear Dynamics | 63_Burgers_Shockwave_Singularity_core.cpp", "EXACT_SOURCE", false, 0xF},
    {64, "phys_064", "#064 64_KPZ_Surface_Growth_Roughness", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 64_KPZ_Surface_Growth_Roughness_core.cpp", "EXACT_SOURCE", false, 0xF},
    {65, "phys_064b_kpz_surface_growth_nq_ny", "64_KPZ_Surface_Growth_NQ_NY", "Physics_Quant_100_240 (Wavelet / Spectral Decoupling) | 64_KPZ_Surface_Growth_NQ_NY_core.cpp", "EXACT_FORMULA", false, 0xC},
    {66, "phys_065", "#065 65_SLE_Conformal_Trace", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {67, "phys_066", "#066 66_Sandpile_Avalanche_Cascade", "Momentum / Dynamic Range Expansion & Breakout | 66_Sandpile_Avalanche_Cascade_core.cpp", "EXACT_SOURCE", false, 0xF},
    {68, "phys_067b_amari_dual_connection_ym", "67_Amari_Dual_Connection_YM", "Physics_Quant_100_240 (Non-Linear Physics) | 67_Amari_Dual_Connection_YM_core.cpp", "EXACT_FORMULA", false, 0xF},
    {69, "phys_067", "#067 67_Amari_Dual_Connection_Flux", "Mathematical Physics & Non-Linear Dynamics | 67_Amari_Dual_Connection_Flux_core.cpp", "EXACT_SOURCE", false, 0xF},
    {70, "phys_068", "#068 68_NonHermitian_Exceptional_Point", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {71, "phys_069", "#069 69_Brusselator_Limit_Cycle", "Mathematical Physics & Non-Linear Dynamics | 69_Brusselator_Limit_Cycle_core.cpp", "EXACT_SOURCE", false, 0xF},
    {72, "phys_070", "#070 70_Fractional_Poisson_Burst", "Momentum / Dynamic Range Expansion & Breakout | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {73, "phys_071", "#071 71_DeSitter_Cosmological_Horizon", "Mathematical Physics & Non-Linear Dynamics | 71_DeSitter_Cosmological_Horizon_core.cpp", "EXACT_SOURCE", false, 0xF},
    {74, "phys_072", "#072 72_Feigenbaum_Chaos_Cascade", "Momentum / Dynamic Range Expansion & Breakout | 72_Feigenbaum_Chaos_Cascade_core.cpp", "EXACT_SOURCE", false, 0xF},
    {75, "phys_073", "#073 73_YangMills_Gauge_Curvature", "Mathematical Physics & Non-Linear Dynamics | 73_YangMills_Gauge_Curvature_core.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {76, "phys_074", "#074 74_NavierStokes_Enstrophy_Jet", "Momentum / Dynamic Range Expansion & Breakout | 74_NavierStokes_Enstrophy_Jet_core.cpp", "EXACT_SOURCE", false, 0xF},
    {77, "phys_075", "#075 75_KdV_Soliton_Envelope", "Momentum / Dynamic Range Expansion & Breakout | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {78, "phys_076", "#076 76_GinzburgLandau_Supercurrent", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {79, "phys_077", "#077 77_Kolmogorov_Turbulence_Cascade", "Momentum / Dynamic Range Expansion & Breakout | 77_Kolmogorov_Turbulence_Cascade_core.cpp", "EXACT_SOURCE", false, 0xF},
    {80, "phys_078", "#078 78_Penrose_Ergosphere_Extraction", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {81, "phys_079", "#079 79_Riemann_Zeta_Resonant_Spacing", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {82, "phys_080", "#080 80_CFT_Virasoro_Central_Charge", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch4_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {83, "phys_081", "#081 81_Casimir_Vacuum_Fluctuation", "Order-Flow & Volume Absorption / Liquidity Sweeps | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {84, "phys_082", "#082 82_Moyal_Bracket_Star_Product", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {85, "phys_083", "#083 83_Ricci_Flow_Metric_Singularity", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {86, "phys_084", "#084 84_Berry_Geometric_Phase_Flux", "Mathematical Physics & Non-Linear Dynamics | 84_Berry_Geometric_Phase_Flux_core.cpp", "EXACT_SOURCE", false, 0xF},
    {87, "phys_085", "#085 85_GinzburgLandau_Vortex_Melting", "Mathematical Physics & Non-Linear Dynamics | 85_GinzburgLandau_Vortex_Melting_core.cpp", "EXACT_SOURCE", false, 0xF},
    {88, "phys_086", "#086 86_AdSCFT_Holographic_Bulk_Current", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {89, "phys_087", "#087 87_Majorana_ZeroMode_Braiding", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {90, "phys_088", "#088 88_NonArchimedean_PAdic_Valuation", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {91, "phys_089", "#089 89_FPUT_Recurrence_Breakdown", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {92, "phys_090", "#090 90_Quantum_Hall_Chiral_Edge", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {93, "phys_091", "#091 91_Fractional_QBM_Entanglement", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {94, "phys_092", "#092 92_GutenbergRichter_SOC_Avalanche", "Mathematical Physics & Non-Linear Dynamics | 92_GutenbergRichter_SOC_Avalanche_core.cpp", "EXACT_SOURCE", false, 0xF},
    {95, "phys_093", "#093 93_RayleighBenard_Convection_Plume", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {96, "phys_094b_nls_peregrine_rogue_ym_ny", "94_NLS_Peregrine_Rogue_YM_NY", "Physics_Quant_100_240 (Non-Linear Physics) | 94_NLS_Peregrine_Rogue_YM_NY_core.cpp", "EXACT_FORMULA", false, 0xC},
    {97, "phys_094", "#094 94_NLS_Peregrine_Rogue_Wave", "Mathematical Physics & Non-Linear Dynamics | 94_NLS_Peregrine_Rogue_Wave_core.cpp", "EXACT_SOURCE", false, 0xF},
    {98, "phys_095", "#095 95_Wigner_Surmise_Spectral_Crossing", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {99, "phys_096", "#096 96_KAM_Torus_Resonance_Overlap", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {100, "phys_097", "#097 97_Chern_Number_Topological_Band", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 97_Chern_Number_Topological_Band_core.cpp", "EXACT_SOURCE", false, 0xF},
    {101, "phys_097b_chern_number_topological_nq", "97_Chern_Number_Topological_NQ", "Physics_Quant_100_240 (Wavelet / Spectral Decoupling) | 97_Chern_Number_Topological_NQ_core.cpp", "EXACT_FORMULA", false, 0xF},
    {102, "phys_098", "#098 98_RankineHugoniot_Shockwave_Jump", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {103, "phys_099", "#099 99_Jarzynski_NonEquilibrium_Work", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {104, "phys_100", "#100 100_Quantum_SpinHall_Helical_Edge", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch5_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {105, "phys_101", "#101 101_CrossAsset_Volatility_Tensor", "Cross-Asset Lead-Lag & Beta Volatility Skew | multiverse_batch6_20strats.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {106, "phys_102", "#102 102_Quantum_Potential_Well_Tunneling", "Mathematical Physics & Non-Linear Dynamics | 102_Quantum_Potential_Well_Tunneling_core.cpp", "EXACT_SOURCE", false, 0xF},
    {107, "phys_103", "#103 103_OrderFlow_Kinetic_Energy_Decel", "Order-Flow & Volume Absorption / Liquidity Sweeps | multiverse_batch6_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {108, "phys_104", "#104 104_Localized_Hurst_Wavelet_Trans", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 104_Localized_Hurst_Wavelet_Trans_core.cpp", "EXACT_SOURCE", false, 0xF},
    {109, "phys_105", "#105 105_Thermodynamic_Gibbs_Phase_Trans", "Mathematical Physics & Non-Linear Dynamics | 105_Thermodynamic_Gibbs_Phase_Trans_core.cpp", "EXACT_SOURCE", false, 0xF},
    {110, "phys_106", "#106 106_EMD_Hilbert_Huang_Phase_Lock", "Mathematical Physics & Non-Linear Dynamics | 106_EMD_Hilbert_Huang_Phase_Lock_core.cpp", "EXACT_SOURCE", false, 0xF},
    {111, "phys_107", "#107 107_Kalman_Innovation_Vector_Surge", "Mathematical Physics & Non-Linear Dynamics | 107_Kalman_Innovation_Vector_Surge_core.cpp", "EXACT_SOURCE", false, 0xF},
    {112, "phys_108", "#108 108_Ornstein_Uhlenbeck_Jump_Escape", "Mean Reversion & Statistical Arbitrage / Boundary Decay | 108_Ornstein_Uhlenbeck_Jump_Escape_core.cpp", "EXACT_SOURCE", false, 0xF},
    {113, "phys_109", "#109 109_Spectral_Wavelet_Coherence_Lead", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | 109_Spectral_Wavelet_Coherence_Lead_core.cpp", "CROSS_ASSET_CAUSAL", true, 0xF},
    {114, "phys_110", "#110 110_Tsallis_NonExtensive_Entropy_Drop", "Multi-Scale Volatility Compression & Spectral Energy Decoupling | multiverse_batch6_20strats.cpp", "EXACT_SOURCE", false, 0xF},
    {115, "phys_111", "#111 111_Stochastic_FastScale_Vol_Asymmetry", "Mathematical Physics & Non-Linear Dynamics | 111_Stochastic_FastScale_Vol_Asymmetry_core.cpp", "EXACT_SOURCE", false, 0xF},
    {116, "phys_112", "#112 112_Microstructural_Block_Absorption", "Order-Flow & Volume Absorption / Liquidity Sweeps | 112_Microstructural_Block_Absorption_core.cpp", "EXACT_SOURCE", false, 0xF},
    {117, "phys_113", "#113 113_Hawkes_SelfExciting_Cluster_Burst", "Momentum / Dynamic Range Expansion & Breakout | 113_Hawkes_SelfExciting_Cluster_Burst_core.cpp", "EXACT_SOURCE", false, 0xF},
    {118, "phys_114", "#114 114_Lyapunov_Exponent_Chaos_Horizon", "Momentum / Dynamic Range Expansion & Breakout | 114_Lyapunov_Exponent_Chaos_Horizon_core.cpp", "EXACT_SOURCE", false, 0xF},
    {119, "phys_115", "#115 115_Fractional_Brownian_Motion_Memory", "Mathematical Physics & Non-Linear Dynamics | multiverse_batch6_20strats.cpp", "EXACT_SOURCE", false, 0xF}
};

inline u11::ParamSpec p_int(const char* name, int def, int mn, int mx, int gp = 5) {
    return {name, static_cast<double>(def), static_cast<double>(mn), static_cast<double>(mx), gp, u11::ParamScale::INT, {}};
}

inline u11::ParamSpec p_lin(const char* name, double def, double mn, double mx, int gp = 5) {
    return {name, def, mn, mx, gp, u11::ParamScale::LINEAR, {}};
}

inline u11::ParamSpec p_log(const char* name, double def, double mn, double mx, int gp = 5) {
    return {name, def, mn, mx, gp, u11::ParamScale::LOG, {}};
}

// Builds Stage-5 sweepable ParamSchema for each of the 120 formulas.
// Tail convention on every schema: last 5 params are always:
//   [n-5]=sl_mult, [n-4]=tp_mult, [n-3]=max_hold, [n-2]=ratchet_trig, [n-1]=ratchet_trail
inline u11::ParamSchema make_batch2_schema(int id) {
    u11::ParamSchema s;
    auto add_atr_tail = [&](double sl_def, double tp_def, int hold_def) {
        s.params.push_back(p_lin("sl_atr", sl_def, std::max(0.4, sl_def * 0.5), sl_def * 2.0));
        s.params.push_back(p_lin("tp_atr", tp_def, std::max(0.8, tp_def * 0.5), tp_def * 1.8));
        s.params.push_back(p_int("max_hold", hold_def, 15, 90));
        s.params.push_back(p_lin("ratchet_trig_atr", 0.0, 0.0, 3.0));
        s.params.push_back(p_lin("ratchet_trail_atr", 0.5, 0.25, 2.0));
    };
    auto add_scale_tail = [&](double sl_def, double tp_def, int hold_def) {
        s.params.push_back(p_lin("sl_pts", sl_def, std::max(5.0, sl_def * 0.5), sl_def * 2.0));
        s.params.push_back(p_lin("tp_pts", tp_def, std::max(10.0, tp_def * 0.5), tp_def * 2.0));
        s.params.push_back(p_int("max_hold", hold_def, 15, 90));
        s.params.push_back(p_lin("ratchet_trig_pts", 0.0, 0.0, 40.0));
        s.params.push_back(p_lin("ratchet_trail_pts", 10.0, 4.0, 25.0));
    };

    switch (id) {
        case 0: // 1_CrossAsset_Imbalance
            s.params = {p_int("lookback", 5, 2, 15), p_lin("es_thresh", 3.0, 1.0, 8.0), p_lin("nq_lag_cap", 2.0, 0.5, 6.0)};
            add_scale_tail(20.0, 35.0, 45); break;
        case 1: // 2_Settlement_MOC
            s.params = {p_int("lookback", 15, 5, 35), p_lin("drift_atr", 1.5, 0.5, 3.5)};
            add_scale_tail(15.0, 25.0, 45); break;
        case 2: // 3_Overnight_Inventory_Flush
            s.params = {p_int("lookback", 60, 20, 120), p_lin("drift_atr", 2.5, 1.0, 5.0)};
            add_scale_tail(20.0, 40.0, 45); break;
        case 3: // 4_Fractal_Hurst_Switcher
            s.params = {p_int("lookback", 20, 8, 45), p_lin("hurst_thresh", 0.45, 0.25, 0.75)};
            add_scale_tail(20.0, 50.0, 45); break;
        case 4: // 5_MultiIndex_Compression
            s.params = {p_lin("comp_atr", 0.40, 0.15, 0.85)};
            add_scale_tail(15.0, 35.0, 45); break;
        case 5: // 6_London_Fix_Expansion
            s.params = {p_int("lookback", 10, 4, 25)};
            add_scale_tail(20.0, 40.0, 45); break;
        case 6: // 7_Volume_Absorption_Exhaustion
            s.params = {p_lin("vol_mult", 2.5, 1.3, 5.0), p_lin("rng_atr_max", 0.50, 0.20, 1.00)};
            add_scale_tail(15.0, 30.0, 45); break;
        case 7: // 8_PostShock_Deceleration
            s.params = {p_lin("shock_atr", 3.0, 1.5, 5.0), p_lin("decel_atr", 1.0, 0.4, 1.8)};
            add_scale_tail(20.0, 40.0, 45); break;
        case 8: // 9_0DTE_Gamma_Pinning
            s.params = {p_lin("strike_step", 50.0, 25.0, 100.0), p_lin("pin_edge", 35.0, 28.0, 45.0)};
            s.params.push_back(p_lin("sl_pts", 15.0, 7.5, 30.0));
            s.params.push_back(p_lin("tp_pin_mult", 1.0, 0.5, 2.0));
            s.params.push_back(p_int("max_hold", 45, 15, 90));
            s.params.push_back(p_lin("ratchet_trig_pts", 0.0, 0.0, 40.0));
            s.params.push_back(p_lin("ratchet_trail_pts", 10.0, 4.0, 25.0));
            break;
        case 9: // 10_Statistical_ZScore_MAD
            s.params = {p_int("lb_mid", 30, 10, 60), p_int("lb_far", 60, 25, 120), p_lin("diff_atr", 2.8, 1.5, 5.0)};
            add_atr_tail(1.2, 2.0, 45); break;
        case 10: // 11_Liquidity_Sweep_Reclaim
            s.params = {p_int("lookback", 16, 6, 35), p_lin("sl_buf_pts", 2.0, 0.5, 6.0)};
            s.params.push_back(p_lin("sl_mult", 1.0, 0.6, 1.8));
            s.params.push_back(p_lin("tp_rr", 2.5, 1.2, 5.0));
            s.params.push_back(p_int("max_hold", 45, 15, 90));
            s.params.push_back(p_lin("ratchet_trig_atr", 0.0, 0.0, 3.0));
            s.params.push_back(p_lin("ratchet_trail_atr", 0.5, 0.25, 2.0));
            break;
        case 11: // 12_MAD_Dynamic_Reversion
            s.params = {p_int("lookback", 30, 10, 60), p_lin("z_thresh", 2.6, 1.5, 4.5)};
            s.params.push_back(p_lin("sl_atr", 1.2, 0.5, 2.5));
            s.params.push_back(p_lin("tp_dev_mult", 1.0, 0.5, 2.0));
            s.params.push_back(p_int("max_hold", 45, 15, 90));
            s.params.push_back(p_lin("ratchet_trig_atr", 0.0, 0.0, 3.0));
            s.params.push_back(p_lin("ratchet_trail_atr", 0.5, 0.25, 2.0));
            break;
        case 12: // 13_Index_Velocity_Divergence
            s.params = {p_int("lookback", 20, 5, 45), p_lin("ret_diff", 0.0035, 0.001, 0.010)};
            add_scale_tail(20.0, 40.0, 45); break;
        case 13: // 14_Volume_Weighted_Entropy
            s.params = {p_int("lookback", 15, 5, 35), p_lin("p_up_thresh", 0.85, 0.65, 0.95)};
            add_atr_tail(1.5, 3.0, 45); break;
        case 14: // 15_Volatility_Imbalance_Shock
            s.params = {p_lin("rng_atr", 3.5, 1.8, 6.0), p_lin("vol_mult", 1.8, 1.1, 3.5), p_lin("body_frac", 0.60, 0.30, 0.85)};
            s.params.push_back(p_lin("sl_rng_mult", 0.5, 0.25, 1.0));
            s.params.push_back(p_lin("tp_rng_mult", 1.0, 0.6, 2.5));
            s.params.push_back(p_int("max_hold", 45, 15, 90));
            s.params.push_back(p_lin("ratchet_trig_atr", 0.0, 0.0, 3.0));
            s.params.push_back(p_lin("ratchet_trail_atr", 0.5, 0.25, 2.0));
            break;
        case 15: // 16_TrueRange_Compression_Trap
            s.params = {p_lin("tr_atr_max", 0.30, 0.10, 0.70)};
            add_scale_tail(15.0, 35.0, 45); break;
        case 16: // 17_Ornstein_Uhlenbeck_Speed
            s.params = {p_lin("dev_atr", 2.5, 1.2, 4.5)};
            add_atr_tail(1.2, 2.0, 45); break;
        case 17: // 18_Kinetic_Energy_Conservation
            s.params = {p_lin("ke_ratio_max", 0.25, 0.05, 0.60)};
            add_scale_tail(18.0, 40.0, 45); break;
        case 18: // 19_POC_Migration_Drift
            s.params = {p_int("lookback", 10, 4, 25), p_lin("drift_atr", 2.0, 1.0, 4.0), p_lin("pullback_atr", 0.30, 0.10, 0.65)};
            add_scale_tail(20.0, 45.0, 45); break;
        case 19: // 20_Asymmetric_Wick_Absorption
            s.params = {p_lin("wick_body_ratio", 2.5, 1.5, 5.0), p_lin("wick_atr_min", 0.40, 0.20, 1.20)};
            s.params.push_back(p_lin("sl_wick_mult", 1.0, 0.5, 2.0));
            s.params.push_back(p_lin("tp_wick_mult", 2.0, 1.0, 4.0));
            s.params.push_back(p_int("max_hold", 45, 15, 90));
            s.params.push_back(p_lin("ratchet_trig_atr", 0.0, 0.0, 3.0));
            s.params.push_back(p_lin("ratchet_trail_atr", 0.5, 0.25, 2.0));
            break;
        case 20: // 21_Hilbert_Instantaneous_Phase
            s.params = {p_int("lookback", 7, 4, 15)};
            add_atr_tail(1.2, 2.4, 40); break;
        case 21: // 22_Kolmogorov_Sinai_Entropy
            s.params = {p_int("lookback", 12, 6, 25), p_lin("entropy_max", 0.40, 0.20, 0.70), p_lin("dom_prob", 0.70, 0.55, 0.90)};
            add_atr_tail(1.5, 3.0, 40); break;
        case 22: // 23_Fisher_Information_Metric
            s.params = {p_int("lookback", 20, 8, 40), p_log("fisher_min", 5e5, 1e5, 2e6), p_log("mean_r_min", 1e-4, 2e-5, 5e-4)};
            add_atr_tail(1.2, 2.5, 40); break;
        case 23: // 24_Kalman_Filter_Optimal_Drift
            s.params = {p_log("kalman_q", 0.01, 0.001, 0.1), p_lin("resid_atr", 2.0, 1.0, 4.0)};
            add_atr_tail(1.0, 2.5, 40); break;
        case 24: // 25_Orderflow_Liquidity_Permeability
            s.params = {p_int("lookback", 10, 4, 25), p_lin("perm_atr", 0.8, 0.2, 2.0)};
            add_atr_tail(1.3, 3.0, 40); break;
        case 25: // 26_Wavelet_Multiresolution_Energy
            s.params = {p_int("lookback", 8, 6, 18), p_lin("energy_ratio", 3.5, 1.5, 6.5), p_lin("disp_atr", 1.5, 0.5, 3.0)};
            add_atr_tail(1.2, 2.8, 40); break;
        case 26: // 27_CrossAsset_Triangular_LeadLag
            s.params = {p_int("lookback", 5, 2, 15), p_lin("lead_spread", 0.0015, 0.0005, 0.0040)};
            add_scale_tail(15.0, 35.0, 40); break;
        case 27: // 28_Hawkes_SelfExciting_Arrivals
            s.params = {p_lin("decay", 0.85, 0.50, 0.95), p_lin("event_atr", 1.8, 1.0, 3.2), p_lin("hawkes_thresh", 2.2, 1.0, 5.0)};
            add_atr_tail(1.4, 3.2, 40); break;
        case 28: // 29_Asymmetric_Jump_Diffusion
            s.params = {p_lin("jump_atr", 2.8, 1.5, 5.0)};
            add_atr_tail(1.0, 2.2, 40); break;
        case 29: // 30_Dynamic_Volume_Sync_VBP
            s.params = {p_lin("vpin_min", 0.75, 0.40, 0.95), p_lin("vol_mult", 1.5, 1.0, 3.0)};
            add_atr_tail(1.3, 2.6, 40); break;
        case 30: // 31_Markov_Regime_Transition_Matrix
            s.params = {p_lin("hvol_atr", 1.4, 0.8, 2.5)};
            add_atr_tail(1.5, 3.5, 40); break;
        case 31: // 32_GARCH_Volatility_Clustering
            s.params = {p_lin("short_atr", 2.0, 1.0, 4.0), p_lin("long_atr", 2.4, 1.2, 4.5)};
            add_atr_tail(1.2, 2.8, 40); break;
        case 32: // 33_Stochastic_Resonance_Pulse
            s.params = {p_int("lookback", 5, 3, 15), p_lin("snr_min", 2.5, 1.2, 5.0), p_lin("sig_atr", 1.5, 0.8, 3.0)};
            add_atr_tail(1.1, 2.4, 40); break;
        case 33: // 34_Topological_Persistence_Homology
            s.params = {p_int("lookback", 10, 5, 25), p_lin("barcode_atr", 3.0, 1.5, 5.0), p_lin("edge_frac", 0.80, 0.65, 0.95)};
            add_atr_tail(1.2, 2.5, 40); break;
        case 34: // 35_Information_Geometric_Curvature
            s.params = {p_int("step_lb", 3, 2, 8), p_lin("curv_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.3, 3.0, 40); break;
        case 35: // 36_Microstructure_BidAsk_Reflectance
            s.params = {p_lin("acoustic_min", 0.05, 0.01, 0.20), p_lin("dp_atr", 1.2, 0.5, 2.5)};
            add_atr_tail(1.0, 2.2, 40); break;
        case 36: // 37_Multifractal_DFA_Spectrum
            s.params = {p_int("lookback", 15, 6, 30), p_lin("kurt_min", 0.40, 0.15, 0.75), p_lin("rng_atr", 1.5, 0.8, 3.0)};
            add_atr_tail(1.4, 3.0, 40); break;
        case 37: // 38_Lyapunov_Exponent_Divergence
            s.params = {p_int("lookback", 10, 5, 25), p_lin("ftle_min", 0.35, 0.10, 0.80)};
            add_atr_tail(1.3, 2.8, 40); break;
        case 38: // 39_Synthetic_Portfolio_Cointegration
            s.params = {p_int("lookback", 30, 10, 60), p_lin("spread_thresh", 25.0, 10.0, 60.0), p_lin("beta", 4.2, 3.0, 5.5)};
            add_scale_tail(15.0, 30.0, 40); break;
        case 39: // 40_Nonlinear_Volterra_Kernel
            s.params = {p_lin("nl_coeff", 0.15, 0.0, 0.50), p_lin("vy_atr", 2.0, 1.0, 4.0)};
            add_atr_tail(1.2, 2.5, 40); break;
        case 40: // 41_Quantum_Tunneling_NQ_AllSess
        case 41: // 41_Quantum_Tunneling_Barrier
            s.params = {p_int("lookback", 5, 3, 12), p_lin("trans_prob", 0.70, 0.40, 0.95), p_lin("ke_ratio", 0.80, 0.40, 1.50)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 42: // 42_Langevin_Stochastic_Drift
            s.params = {p_int("lb_vel", 10, 4, 20), p_int("lb_pot", 20, 10, 40), p_lin("gamma", 0.15, 0.05, 0.50), p_lin("force_min", 1.8, 0.8, 3.5)};
            add_atr_tail(1.2, 2.5, 40); break;
        case 43: // 43_EMD_Hilbert_Huang_Frequency
            s.params = {p_lin("amp_atr", 1.8, 0.8, 3.5), p_lin("deriv_atr", 0.50, 0.20, 1.50)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 44: // 44_Renyi_Entropy_OrderQ_Spectrum
            s.params = {p_int("lookback", 15, 6, 30), p_lin("skew_min", 0.35, 0.15, 0.75), p_lin("floor_prob", 0.10, 0.05, 0.30)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 45: // 45_Fokker_Planck_Probability_Flux
            s.params = {p_int("lb_mu", 15, 6, 30), p_int("lb_grad", 5, 2, 12), p_lin("flux_atr", 0.80, 0.30, 2.00)};
            add_atr_tail(1.15, 2.6, 40); break;
        case 46: // 46_Betti_Number_Zero_Persistence
            s.params = {p_int("step_lb", 6, 3, 12), p_lin("d01_atr", 1.5, 0.8, 3.0), p_lin("d12_atr", 0.5, 0.2, 1.0)};
            add_atr_tail(1.1, 2.5, 40); break;
        case 47: // 47_Hawkes_Modulated_Jump_Drift
            s.params = {p_lin("decay", 0.85, 0.50, 0.95), p_lin("hawkes_thresh", 2.2, 1.0, 4.5)};
            add_atr_tail(1.2, 2.6, 40); break;
        case 48: // 48_Kuramoto_Phase_Synchronization
            s.params = {p_int("lookback", 5, 2, 12), p_lin("sync_atr", 0.80, 0.30, 2.00)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 49: // 49_Thermodynamic_Landauer_Work
            s.params = {p_int("lookback", 10, 4, 25), p_lin("work_thresh", 8.0, 3.0, 18.0)};
            add_atr_tail(1.15, 2.55, 40); break;
        case 50: // 50_Fractional_Brownian_Hurst_Field
            s.params = {p_int("lookback", 10, 5, 20), p_lin("h_thresh", 0.72, 0.55, 0.95)};
            add_atr_tail(1.1, 2.7, 40); break;
        case 51: // 51_Koopman_DMD_Leading_Mode
            s.params = {p_int("lookback", 15, 6, 30), p_lin("koopman_min", 1.08, 1.01, 1.30), p_lin("disp_atr", 1.5, 0.8, 3.0)};
            add_atr_tail(1.15, 2.65, 40); break;
        case 52: // 52_Tsallis_NonExtensive_Tail
            s.params = {p_int("lookback", 20, 8, 40), p_lin("tail_atr", 2.4, 1.2, 4.5)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 53: // 53_Lie_Algebra_Hyperbolic_Shear
            s.params = {p_int("lookback", 10, 4, 25), p_lin("shear_min", 3.0, 1.5, 6.0), p_lin("dx_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.15, 2.6, 40); break;
        case 54: // 54_Graph_Laplacian_Fiedler_Cut
            s.params = {p_int("lb1", 10, 4, 20), p_int("lb2", 20, 10, 40), p_lin("fiedler_atr", 2.0, 1.0, 4.0)};
            add_atr_tail(1.1, 2.55, 40); break;
        case 55: // 55_CWT_Morlet_Ridge_Tracking
            s.params = {p_int("lookback", 12, 6, 24), p_lin("ridge_power", 12.0, 4.0, 30.0)};
            add_atr_tail(1.15, 2.6, 40); break;
        case 56: // 56_Wasserstein_EarthMover_Flux
            s.params = {p_int("lookback", 10, 4, 20), p_lin("wass_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 57: // 57_Volterra_Memory_Dissipation
            s.params = {p_int("lookback", 15, 6, 30), p_lin("mem_decay", 0.20, 0.05, 0.50), p_lin("mem_atr", 1.6, 0.8, 3.2)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 58: // 58_Poincare_Map_Tangent_Divergence
            s.params = {p_int("lookback", 5, 3, 12), p_lin("slope_min", 2.2, 1.2, 4.0), p_lin("xn_atr", 1.4, 0.6, 2.8)};
            add_atr_tail(1.15, 2.65, 40); break;
        case 59: // 59_Girsanov_Measure_Change_Shift
            s.params = {p_int("lookback", 20, 8, 40), p_lin("theta_min", 2.5, 1.2, 4.5)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 60: // 60_Quantum_Harmonic_Displacement
            s.params = {p_int("lookback", 12, 5, 25), p_lin("disp_atr", 2.2, 1.0, 4.0)};
            add_atr_tail(1.1, 2.6, 40); break;
        case 61: // 61_Relativistic_Lorentz_Flow
            s.params = {p_lin("c_mkt_atr", 3.5, 2.0, 6.0), p_lin("gamma_min", 1.35, 1.05, 2.5), p_lin("beta_min", 0.55, 0.30, 0.80)};
            add_atr_tail(1.0, 2.9, 45); break;
        case 62: // 62_SpinGlass_Frustration_Relaxation
            s.params = {p_int("lookback", 12, 6, 25), p_lin("mag_min", 0.58, 0.30, 0.90)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 63: // 63_Burgers_Shockwave_Singularity
            s.params = {p_int("lb_front", 3, 2, 6), p_lin("shock_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 64: // 64_KPZ_Surface_Growth_Roughness
        case 65: // 64_KPZ_Surface_Growth_NQ_NY
            s.params = {p_int("lookback", 15, 6, 30), p_lin("rough_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.05, 2.75, 45); break;
        case 66: // 65_SLE_Conformal_Trace
            s.params = {p_int("lookback", 10, 4, 20), p_lin("kappa_min", 4.2, 2.0, 6.5), p_lin("kappa_max", 8.0, 6.5, 14.0)};
            add_atr_tail(1.0, 2.8, 45); break;
        case 67: // 66_Sandpile_Avalanche_Cascade
            s.params = {p_int("lookback", 4, 2, 10), p_lin("decay", 0.80, 0.50, 0.95), p_lin("slope_thresh", 1.7, 0.8, 3.2)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 68: // 67_Amari_Dual_Connection_YM
        case 69: // 67_Amari_Dual_Connection_Flux
            s.params = {p_int("lb_e", 5, 2, 12), p_int("lb_m", 15, 6, 30), p_lin("flux_thresh", 2.0, 1.0, 4.0)};
            add_atr_tail(1.05, 2.75, 45); break;
        case 70: // 68_NonHermitian_Exceptional_Point
            s.params = {p_int("step_lb", 5, 2, 10), p_lin("ep_max", 0.25, 0.05, 0.60), p_lin("dw_min", 1.4, 0.6, 2.8)};
            add_atr_tail(1.0, 2.9, 45); break;
        case 71: // 69_Brusselator_Limit_Cycle
            s.params = {p_int("lookback", 8, 4, 20), p_lin("rad_min", 2.2, 1.0, 4.5), p_lin("x_min", 1.5, 0.6, 3.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 72: // 70_Fractional_Poisson_Burst
            s.params = {p_int("lookback", 10, 5, 20), p_int("min_bursts", 4, 2, 8), p_lin("jump_atr", 0.80, 0.40, 1.50)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 73: // 71_DeSitter_Cosmological_Horizon
            s.params = {p_int("lookback", 10, 4, 20), p_lin("hubble_min", 0.20, 0.05, 0.60), p_lin("disp_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 74: // 72_Feigenbaum_Chaos_Cascade
            s.params = {p_int("lookback", 5, 2, 12), p_lin("r_min", 2.1, 1.2, 3.2), p_lin("r_max", 4.0, 3.2, 6.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 75: // 73_YangMills_Gauge_Curvature
            s.params = {p_int("lookback", 3, 2, 10), p_lin("curl_min", 1.2, 0.5, 2.5), p_lin("nq_min", 1.4, 0.6, 2.8)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 76: // 74_NavierStokes_Enstrophy_Jet
            s.params = {p_lin("decay", 0.80, 0.50, 0.95), p_lin("enstrophy_atr2", 2.0, 0.8, 4.5), p_lin("disp_atr", 1.5, 0.6, 3.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 77: // 75_KdV_Soliton_Envelope
            s.params = {p_int("lookback", 6, 3, 14), p_lin("sech_min", 1.8, 0.8, 3.5), p_lin("curv_max", 0.35, 0.10, 0.80)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 78: // 76_GinzburgLandau_Supercurrent
            s.params = {p_int("lb_mod", 15, 6, 30), p_int("lb_grad", 3, 2, 8), p_lin("j_min", 2.6, 1.2, 5.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 79: // 77_Kolmogorov_Turbulence_Cascade
            s.params = {p_int("lookback", 5, 2, 12), p_lin("du3_min", 6.0, 2.0, 15.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 80: // 78_Penrose_Ergosphere_Extraction
            s.params = {p_int("lookback", 10, 4, 20), p_lin("ergo_min", 1.6, 0.8, 3.0), p_lin("esc_min", 0.9, 0.4, 2.0)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 81: // 79_Riemann_Zeta_Resonant_Spacing
            s.params = {p_int("lookback", 7, 4, 16), p_lin("ratio_min", 2.2, 1.2, 4.0), p_lin("disp_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 82: // 80_CFT_Virasoro_Central_Charge
            s.params = {p_int("lookback", 15, 6, 30), p_lin("c_ratio_min", 0.65, 0.35, 0.90), p_lin("ir_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 83: // 81_Casimir_Vacuum_Fluctuation
            s.params = {p_int("lookback", 5, 2, 12), p_lin("gap_max", 0.40, 0.15, 0.75), p_lin("casimir_min", 35.0, 10.0, 80.0), p_lin("disp_atr", 0.8, 0.3, 1.8)};
            add_atr_tail(1.0, 2.9, 45); break;
        case 84: // 82_Moyal_Bracket_Star_Product
            s.params = {p_lin("moyal_min", 1.8, 0.8, 4.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 85: // 83_Ricci_Flow_Metric_Singularity
            s.params = {p_int("lookback", 9, 5, 19), p_lin("curv_min", 1.9, 0.8, 3.5), p_lin("shrink_min", 1.4, 0.6, 3.0)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 86: // 84_Berry_Geometric_Phase_Flux
            s.params = {p_int("lookback", 8, 4, 18), p_lin("berry_min", 2.5, 1.0, 5.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 87: // 85_GinzburgLandau_Vortex_Melting
            s.params = {p_int("lookback", 6, 3, 14), p_lin("decay", 0.85, 0.50, 0.95), p_lin("vortex_min", 2.1, 1.0, 4.0), p_lin("disp_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 88: // 86_AdSCFT_Holographic_Bulk_Current
            s.params = {p_int("lb_ir", 20, 8, 40), p_int("lb_uv", 3, 2, 8), p_lin("bulk_min", 2.4, 1.0, 5.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 89: // 87_Majorana_ZeroMode_Braiding
            s.params = {p_int("step_lb", 5, 3, 10), p_lin("braid_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.0, 2.8, 45); break;
        case 90: // 88_NonArchimedean_PAdic_Valuation
            s.params = {p_int("lb_3", 4, 2, 8), p_int("lb_9", 10, 5, 20), p_lin("padic_min", 2.6, 1.4, 4.5), p_lin("d9_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 91: // 89_FPUT_Recurrence_Breakdown
            s.params = {p_int("lookback", 7, 4, 15), p_lin("energy_min", 4.5, 2.0, 9.0), p_lin("mode1_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 92: // 90_Quantum_Hall_Chiral_Edge
            s.params = {p_int("lookback", 5, 2, 12), p_lin("edge_atr", 1.4, 0.6, 2.8)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 93: // 91_Fractional_QBM_Entanglement
            s.params = {p_int("lookback", 8, 4, 20), p_lin("hurst_min", 0.62, 0.40, 0.85), p_lin("disp_atr", 1.7, 0.8, 3.2)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 94: // 92_GutenbergRichter_SOC_Avalanche
            s.params = {p_int("lookback", 8, 4, 18), p_lin("slip_min", 1.8, 0.8, 3.5), p_lin("disp_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 95: // 93_RayleighBenard_Convection_Plume
            s.params = {p_int("lookback", 12, 5, 25), p_lin("decay", 0.80, 0.50, 0.95), p_lin("conv_min", 1.8, 0.8, 3.5)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 96: // 94_NLS_Peregrine_Rogue_YM_NY
        case 97: // 94_NLS_Peregrine_Rogue_Wave
            s.params = {p_int("lookback", 5, 2, 12), p_lin("rogue_mult", 2.8, 1.5, 5.0), p_lin("amp_min", 1.8, 0.8, 3.5)};
            add_atr_tail(1.0, 2.9, 45); break;
        case 98: // 95_Wigner_Surmise_Spectral_Crossing
            s.params = {p_int("lookback", 6, 3, 14), p_lin("repul_max", 0.15, 0.03, 0.40), p_lin("disp_atr", 1.6, 0.8, 3.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 99: // 96_KAM_Torus_Resonance_Overlap
            s.params = {p_int("lookback", 10, 5, 20), p_lin("chirikov_min", 2.8, 1.2, 5.5)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 100: // 97_Chern_Number_Topological_Band
        case 101: // 97_Chern_Number_Topological_NQ
            s.params = {p_int("lookback", 7, 4, 16), p_lin("chern_min", 2.2, 1.0, 4.5), p_lin("kx_min", 1.4, 0.6, 2.8)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 102: // 98_RankineHugoniot_Shockwave_Jump
            s.params = {p_int("lookback", 6, 3, 14), p_lin("jump_mult", 1.8, 1.1, 3.5), p_lin("post_min", 1.6, 0.8, 3.0)};
            add_atr_tail(1.0, 2.9, 45); break;
        case 103: // 99_Jarzynski_NonEquilibrium_Work
            s.params = {p_int("lookback", 6, 3, 15), p_lin("work_min", 9.0, 3.5, 20.0)};
            add_atr_tail(1.05, 2.8, 45); break;
        case 104: // 100_Quantum_SpinHall_Helical_Edge
            s.params = {p_int("lookback", 12, 6, 24), p_lin("spin_min", 1.5, 0.6, 3.0)};
            add_atr_tail(1.0, 2.85, 45); break;
        case 105: // 101_CrossAsset_Volatility_Tensor
            s.params = {p_int("lookback", 4, 2, 12), p_lin("lead_atr", 1.8, 0.8, 3.5), p_lin("lag_cap_atr", 0.8, 0.2, 1.8)};
            add_atr_tail(0.9, 4.5, 45); break;
        case 106: // 102_Quantum_Potential_Well_Tunneling
            s.params = {p_int("lookback", 20, 8, 40), p_lin("dev_min", 1.6, 0.8, 3.2)};
            add_atr_tail(0.8, 4.2, 45); break;
        case 107: // 103_OrderFlow_Kinetic_Energy_Decel
            s.params = {p_int("vol_lb", 10, 4, 20), p_lin("accel_max", 0.20, 0.05, 0.60), p_lin("vel_min", 1.8, 0.8, 3.5), p_lin("vol_ratio", 1.5, 1.0, 3.0)};
            add_atr_tail(0.85, 4.0, 45); break;
        case 108: // 104_Localized_Hurst_Wavelet_Trans
            s.params = {p_int("lookback", 16, 8, 32), p_lin("hurst_min", 0.68, 0.50, 0.95)};
            add_atr_tail(0.9, 4.5, 45); break;
        case 109: // 105_Thermodynamic_Gibbs_Phase_Trans
            s.params = {p_int("lookback", 5, 2, 12), p_lin("temp_scale", 1000.0, 250.0, 2500.0), p_lin("gibbs_thresh", 3000.0, 1000.0, 8000.0)};
            add_atr_tail(0.95, 4.2, 45); break;
        case 110: // 106_EMD_Hilbert_Huang_Phase_Lock
            s.params = {p_lin("imf1_min", 0.5, 0.2, 1.5), p_lin("imf2_min", 1.0, 0.4, 2.5)};
            add_atr_tail(0.8, 4.0, 45); break;
        case 111: // 107_Kalman_Innovation_Vector_Surge
            s.params = {p_log("q_proc", 0.05, 0.005, 0.25), p_lin("r_mult", 0.25, 0.05, 1.0), p_lin("innov_atr", 2.0, 1.0, 4.0)};
            add_atr_tail(0.9, 4.5, 45); break;
        case 112: // 108_Ornstein_Uhlenbeck_Jump_Escape
            s.params = {p_lin("ou_alpha", 0.05, 0.01, 0.25), p_lin("jump_rng_atr", 2.5, 1.2, 4.5)};
            add_atr_tail(0.85, 4.2, 45); break;
        case 113: // 109_Spectral_Wavelet_Coherence_Lead
            s.params = {p_int("lookback", 6, 3, 15), p_lin("es_atr_min", 2.2, 1.0, 4.0), p_lin("nq_atr_max", 1.0, 0.3, 2.0)};
            add_atr_tail(0.9, 4.0, 45); break;
        case 114: // 110_Tsallis_NonExtensive_Entropy_Drop
            s.params = {p_int("lookback", 10, 5, 20), p_lin("q_order", 1.5, 1.1, 2.5), p_lin("tsallis_max", 0.25, 0.10, 0.55), p_lin("dom_prob", 0.75, 0.60, 0.90)};
            add_atr_tail(0.85, 4.2, 45); break;
        case 115: // 111_Stochastic_FastScale_Vol_Asymmetry
            s.params = {p_lin("fast_vol_atr", 2.5, 1.2, 5.0)};
            add_atr_tail(1.0, 4.5, 45); break;
        case 116: // 112_Microstructural_Block_Absorption
            s.params = {p_int("lookback", 5, 2, 15), p_lin("dens_mult", 3.0, 1.5, 6.0), p_lin("rng_max_atr", 0.60, 0.25, 1.20)};
            add_atr_tail(0.75, 4.2, 45); break;
        case 117: // 113_Hawkes_SelfExciting_Cluster_Burst
            s.params = {p_lin("decay", 0.85, 0.50, 0.95), p_lin("hawkes_min", 2.8, 1.2, 5.0), p_lin("dom_ratio", 3.0, 1.5, 6.0)};
            add_atr_tail(0.95, 4.2, 45); break;
        case 118: // 114_Lyapunov_Exponent_Chaos_Horizon
            s.params = {p_int("lookback", 4, 2, 10), p_lin("lyap_min", 0.80, 0.30, 1.80), p_lin("d1_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(0.9, 4.0, 45); break;
        case 119: // 115_Fractional_Brownian_Motion_Memory
        default:
            s.params = {p_lin("fdiff_atr", 1.8, 0.8, 3.5)};
            add_atr_tail(0.95, 4.5, 45); break;
    }
    return s;
}

struct Batch2State {
    int formula_id = 0;
    int32_t session_mask = 0xF;
    u11::InstrumentSpec inst{};
    double inst_ratio = 1.0;

    // Raw schema values + extracted tail
    double p[16] = {};
    int n_params = 0;
    double sl_mult = 1.2;
    double tp_mult = 2.5;
    int max_hold = 45;
    double ratch_trig = 0.0;
    double ratch_trail = 0.5;
    int req_bars = 20;

    // Causal incremental filter states (for the 10 stateful strategies)
    bool state_init = false;
    double kalman_x = 0.0;
    double kalman_p = 1.0;
    double filter_val = 0.0;
    double hawkes_pos = 0.0;
    double hawkes_neg = 0.0;

    void init(int id, const std::vector<double>& pvals, const u11::InstrumentSpec* spec) {
        formula_id = (id >= 0 && id < 120) ? id : 0;
        session_mask = kBatch2Meta[formula_id].session_mask;
        inst = spec ? *spec : u11::get_instrument_spec("NQ");
        inst_ratio = inst.baseline_index / 4524.25;

        u11::ParamSchema schema = make_batch2_schema(formula_id);
        const std::vector<double> defs = schema.defaults();
        n_params = std::min(16, static_cast<int>(defs.size()));
        for (int i = 0; i < 16; ++i) p[i] = 0.0;
        for (int i = 0; i < n_params; ++i) {
            double raw = (i < static_cast<int>(pvals.size())) ? pvals[static_cast<size_t>(i)] : defs[static_cast<size_t>(i)];
            p[i] = schema.params[static_cast<size_t>(i)].clamp_and_snap(raw);
        }

        if (n_params >= 5) {
            sl_mult = p[n_params - 5];
            tp_mult = p[n_params - 4];
            max_hold = std::max(5, static_cast<int>(std::lround(p[n_params - 3])));
            ratch_trig = p[n_params - 2];
            ratch_trail = p[n_params - 1];
        }

        state_init = false;
        kalman_x = 0.0;
        kalman_p = 1.0;
        filter_val = 0.0;
        hawkes_pos = 0.0;
        hawkes_neg = 0.0;

        // Compute conservative causal history requirement based on integer params
        int mx_lb = 20;
        for (int i = 0; i < n_params - 5; ++i) {
            if (schema.params[static_cast<size_t>(i)].scale == u11::ParamScale::INT) {
                mx_lb = std::max(mx_lb, static_cast<int>(std::lround(p[i])) * 3 + 4);
            }
        }
        req_bars = std::max(20, mx_lb);
    }

    int p_int(int idx) const {
        return std::max(1, static_cast<int>(std::lround(p[idx])));
    }
};

// Cross-asset causal accessors (respecting primary symbol if ref_* not redundantly attached)
inline bool ca_has_nq(const u11::CausalBarView& v, const Batch2State& st) {
    return v.has_ref_nq() || (st.inst.symbol == "NQ");
}
inline bool ca_has_es(const u11::CausalBarView& v, const Batch2State& st) {
    return v.has_ref_es() || (st.inst.symbol == "ES");
}
inline bool ca_has_ym(const u11::CausalBarView& v, const Batch2State& st) {
    return v.has_ref_ym() || (st.inst.symbol == "YM");
}
inline const u11::RawBar& ca_nq_bar(const u11::CausalBarView& v, const Batch2State& st, int lag) {
    (void)st;
    return v.has_ref_nq() ? v.ref_nq_bar(lag) : v[lag];
}
inline const u11::RawBar& ca_es_bar(const u11::CausalBarView& v, const Batch2State& st, int lag) {
    (void)st;
    return v.has_ref_es() ? v.ref_es_bar(lag) : v[lag];
}
inline const u11::RawBar& ca_ym_bar(const u11::CausalBarView& v, const Batch2State& st, int lag) {
    (void)st;
    return v.has_ref_ym() ? v.ref_ym_bar(lag) : v[lag];
}
inline double ca_nq_atr(const u11::CausalBarView& v, const Batch2State& st, int lag) {
    (void)st;
    return v.has_ref_nq() ? v.ref_nq_atr14(lag) : v.atr14(lag);
}
inline double ca_es_atr(const u11::CausalBarView& v, const Batch2State& st, int lag) {
    (void)st;
    return v.has_ref_es() ? v.ref_es_atr14(lag) : v.atr14(lag);
}
inline double ca_ym_atr(const u11::CausalBarView& v, const Batch2State& st, int lag) {
    (void)st;
    return v.has_ref_ym() ? v.ref_ym_atr14(lag) : v.atr14(lag);
}

inline void update_stateful_filters(const u11::CausalBarView& view, Batch2State& st, double cur_atr) {
    switch (st.formula_id) {
        case 23: { // 24_Kalman_Filter_Optimal_Drift
            if (!st.state_init) {
                st.kalman_x = static_cast<double>(view[0].c);
                st.kalman_p = 1.0;
                st.state_init = true;
            }
            double q_kal = st.p[0];
            double r_kal = cur_atr * cur_atr;
            st.kalman_p += q_kal;
            double k_gain = st.kalman_p / (st.kalman_p + r_kal + 1e-9);
            st.kalman_x = st.kalman_x + k_gain * (static_cast<double>(view[0].c) - st.kalman_x);
            st.kalman_p = (1.0 - k_gain) * st.kalman_p;
            break;
        }
        case 27: { // 28_Hawkes_SelfExciting_Arrivals
            bool is_ev = (static_cast<double>(view[0].h) - static_cast<double>(view[0].l)) > st.p[1] * cur_atr;
            st.filter_val = st.filter_val * st.p[0] + (is_ev ? 1.0 : 0.0);
            break;
        }
        case 47: { // 47_Hawkes_Modulated_Jump_Drift
            double jump = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            st.filter_val = st.p[0] * st.filter_val + 0.5 * jump;
            break;
        }
        case 67: { // 66_Sandpile_Avalanche_Cascade
            int lb = std::max(2, st.p_int(0));
            double slope = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            st.filter_val = st.p[1] * st.filter_val + 0.3 * slope;
            break;
        }
        case 76: { // 74_NavierStokes_Enstrophy_Jet
            double vort = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c))
                        - (static_cast<double>(view[-2].c) - static_cast<double>(view[-3].c));
            st.filter_val = st.p[0] * st.filter_val + (1.0 - st.p[0]) * (vort * vort);
            break;
        }
        case 87: { // 85_GinzburgLandau_Vortex_Melting
            double flux = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-3].c)) / cur_atr;
            st.filter_val = st.p[1] * st.filter_val + 0.25 * flux;
            break;
        }
        case 95: { // 93_RayleighBenard_Convection_Plume
            int lb = std::max(2, st.p_int(0));
            double dt = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            st.filter_val = st.p[1] * st.filter_val + 0.3 * dt;
            break;
        }
        case 111: { // 107_Kalman_Innovation_Vector_Surge
            if (!st.state_init) {
                st.kalman_x = static_cast<double>(view[0].c);
                st.kalman_p = 1.0;
                st.state_init = true;
            }
            double r_meas = cur_atr * cur_atr * st.p[1];
            double q_proc = st.p[0];
            st.kalman_p += q_proc;
            double k_gain = st.kalman_p / (st.kalman_p + r_meas + 1e-9);
            double innov = static_cast<double>(view[0].c) - st.kalman_x;
            st.kalman_x += k_gain * innov;
            st.kalman_p *= (1.0 - k_gain);
            st.filter_val = innov;
            break;
        }
        case 112: { // 108_Ornstein_Uhlenbeck_Jump_Escape
            if (!st.state_init) {
                st.filter_val = static_cast<double>(view[0].c);
                st.state_init = true;
            }
            double alpha = st.p[0];
            st.filter_val = (1.0 - alpha) * st.filter_val + alpha * static_cast<double>(view[0].c);
            break;
        }
        case 117: { // 113_Hawkes_SelfExciting_Cluster_Burst
            st.hawkes_pos *= st.p[0];
            st.hawkes_neg *= st.p[0];
            if (view[0].c > view[0].o) {
                st.hawkes_pos += (static_cast<double>(view[0].c) - static_cast<double>(view[0].o)) / cur_atr;
            } else {
                st.hawkes_neg += (static_cast<double>(view[0].o) - static_cast<double>(view[0].c)) / cur_atr;
            }
            break;
        }
        default:
            break;
    }
}

inline void eval_group_001_020(
    const u11::CausalBarView& view, const Batch2State& st,
    double cur_atr, double scale,
    int8_t& dir, double& sl_dist, double& tp_dist, double& r_unit
) {
    const double ir = st.inst_ratio;
    const int16_t et_min = view.et_mins();
    auto set_scale_sl_tp = [&]() {
        sl_dist = st.sl_mult * scale * ir;
        tp_dist = st.tp_mult * scale * ir;
        r_unit = scale * ir;
    };
    auto set_atr_sl_tp = [&]() {
        sl_dist = st.sl_mult * cur_atr;
        tp_dist = st.tp_mult * cur_atr;
        r_unit = cur_atr;
    };

    switch (st.formula_id) {
        case 0: { // 1_CrossAsset_Imbalance
            if (!ca_has_es(view, st) || !ca_has_nq(view, st)) return;
            int lb = std::max(2, st.p_int(0));
            double es_mom = static_cast<double>(ca_es_bar(view, st, 0).c) - static_cast<double>(ca_es_bar(view, st, -(lb - 1)).c);
            double nq_mom = static_cast<double>(ca_nq_bar(view, st, 0).c) - static_cast<double>(ca_nq_bar(view, st, -(lb - 1)).c);
            if (es_mom > st.p[1] && nq_mom < st.p[2]) { dir = 1; set_scale_sl_tp(); }
            else if (es_mom < -st.p[1] && nq_mom > -st.p[2]) { dir = -1; set_scale_sl_tp(); }
            break;
        }
        case 1: { // 2_Settlement_MOC (15:45 - 15:58 ET)
            if (et_min >= 945 && et_min <= 958) {
                int lb = std::max(2, st.p_int(0));
                double drift = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
                if (drift > st.p[1] * cur_atr) { dir = 1; set_scale_sl_tp(); }
                else if (drift < -st.p[1] * cur_atr) { dir = -1; set_scale_sl_tp(); }
            }
            break;
        }
        case 2: { // 3_Overnight_Inventory_Flush (09:30 - 09:45 ET)
            if (et_min >= 570 && et_min <= 585) {
                int lb = std::max(2, st.p_int(0));
                double on_drift = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
                if (on_drift > st.p[1] * cur_atr) { dir = -1; set_scale_sl_tp(); }
                else if (on_drift < -st.p[1] * cur_atr) { dir = 1; set_scale_sl_tp(); }
            }
            break;
        }
        case 3: { // 4_Fractal_Hurst_Switcher
            int lb = std::max(2, st.p_int(0));
            double tr_sum = 0.0;
            for (int k = 0; k < lb; ++k) {
                tr_sum += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c));
            }
            double net_move = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c));
            double hurst = (tr_sum > 1e-9) ? (net_move / tr_sum) : 0.0;
            if (hurst > st.p[1]) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_scale_sl_tp();
            }
            break;
        }
        case 4: { // 5_MultiIndex_Compression
            if (!ca_has_nq(view, st) || !ca_has_es(view, st) || !ca_has_ym(view, st)) return;
            double a0 = std::max(1e-6, ca_nq_atr(view, st, 0));
            double a1 = std::max(1e-6, ca_es_atr(view, st, 0));
            double a2 = std::max(1e-6, ca_ym_atr(view, st, 0));
            double c_nq = (static_cast<double>(ca_nq_bar(view, st, 0).h) - static_cast<double>(ca_nq_bar(view, st, 0).l)) / a0;
            double c_es = (static_cast<double>(ca_es_bar(view, st, 0).h) - static_cast<double>(ca_es_bar(view, st, 0).l)) / a1;
            double c_ym = (static_cast<double>(ca_ym_bar(view, st, 0).h) - static_cast<double>(ca_ym_bar(view, st, 0).l)) / a2;
            if (c_nq < st.p[0] && c_es < st.p[0] && c_ym < st.p[0]) {
                if (view[0].c > view[-1].h) { dir = 1; set_scale_sl_tp(); }
                else if (view[0].c < view[-1].l) { dir = -1; set_scale_sl_tp(); }
            }
            break;
        }
        case 5: { // 6_London_Fix_Expansion (03:55 - 04:15 ET)
            if (et_min >= 235 && et_min <= 255) {
                int lb = std::max(2, st.p_int(0));
                if (view[0].c > view[-(lb - 1)].h) { dir = 1; set_scale_sl_tp(); }
                else if (view[0].c < view[-(lb - 1)].l) { dir = -1; set_scale_sl_tp(); }
            }
            break;
        }
        case 6: { // 7_Volume_Absorption_Exhaustion
            double rng = static_cast<double>(view[0].h) - static_cast<double>(view[0].l);
            if (static_cast<double>(view[0].v) > st.p[0] * static_cast<double>(view[-1].v) && rng < st.p[1] * cur_atr) {
                dir = (view[0].c < view[0].o) ? 1 : -1;
                set_scale_sl_tp();
            }
            break;
        }
        case 7: { // 8_PostShock_Deceleration
            double r_prev = static_cast<double>(view[-1].h) - static_cast<double>(view[-1].l);
            double r_cur = static_cast<double>(view[0].h) - static_cast<double>(view[0].l);
            if (r_prev > st.p[0] * cur_atr && r_cur < st.p[1] * cur_atr) {
                dir = (view[-1].c > view[-1].o) ? -1 : 1;
                set_scale_sl_tp();
            }
            break;
        }
        case 8: { // 9_0DTE_Gamma_Pinning (14:00 - 16:00 ET)
            if (et_min >= 840 && et_min <= 960) {
                double step = st.p[0] * ir;
                if (step <= 1e-6) return;
                double sd = std::fmod(static_cast<double>(view[0].c), step);
                if (sd < 0.0) sd += step;
                double hi_edge = (st.p[1] / 50.0) * step;
                double lo_edge = step - hi_edge;
                if (sd > hi_edge) {
                    dir = 1;
                    sl_dist = st.sl_mult * scale * ir;
                    tp_dist = std::max(st.inst.tick_size * 4.0, (step - sd) * st.tp_mult);
                    r_unit = scale * ir;
                } else if (sd < lo_edge && sd > 0.0) {
                    dir = -1;
                    sl_dist = st.sl_mult * scale * ir;
                    tp_dist = std::max(st.inst.tick_size * 4.0, sd * st.tp_mult);
                    r_unit = scale * ir;
                }
            }
            break;
        }
        case 9: { // 10_Statistical_ZScore_MAD
            int lb1 = std::max(2, st.p_int(0));
            int lb2 = std::max(lb1 + 1, st.p_int(1));
            double mean = (static_cast<double>(view[0].c) + static_cast<double>(view[-(lb1 - 1)].c) + static_cast<double>(view[-(lb2 - 1)].c)) / 3.0;
            double diff = static_cast<double>(view[0].c) - mean;
            if (std::abs(diff) > st.p[2] * cur_atr) {
                dir = (diff > 0.0) ? -1 : 1;
                set_atr_sl_tp();
            }
            break;
        }
        case 10: { // 11_Liquidity_Sweep_Reclaim
            int lb = std::max(3, st.p_int(0));
            double h15 = static_cast<double>(view[-1].h);
            double l15 = static_cast<double>(view[-1].l);
            for (int k = 2; k < lb; ++k) {
                h15 = std::max(h15, static_cast<double>(view[-k].h));
                l15 = std::min(l15, static_cast<double>(view[-k].l));
            }
            double buf = st.p[1] * ir;
            if (static_cast<double>(view[0].h) > h15 && static_cast<double>(view[0].c) < h15) {
                dir = -1;
                double raw_sl = std::max(2.0 * ir, (static_cast<double>(view[0].h) - static_cast<double>(view[0].c)) + buf);
                sl_dist = raw_sl * st.sl_mult;
                tp_dist = sl_dist * st.tp_mult;
                r_unit = cur_atr;
            } else if (static_cast<double>(view[0].l) < l15 && static_cast<double>(view[0].c) > l15) {
                dir = 1;
                double raw_sl = std::max(2.0 * ir, (static_cast<double>(view[0].c) - static_cast<double>(view[0].l)) + buf);
                sl_dist = raw_sl * st.sl_mult;
                tp_dist = sl_dist * st.tp_mult;
                r_unit = cur_atr;
            }
            break;
        }
        case 11: { // 12_MAD_Dynamic_Reversion
            int lb = std::max(5, st.p_int(0));
            double sum = 0.0;
            for (int k = 0; k < lb; ++k) sum += static_cast<double>(view[-k].c);
            double med = sum / static_cast<double>(lb);
            double mad = 0.0;
            for (int k = 0; k < lb; ++k) mad += std::abs(static_cast<double>(view[-k].c) - med);
            mad /= static_cast<double>(lb);
            if (mad > 1e-9) {
                double z = (static_cast<double>(view[0].c) - med) / (1.4826 * mad);
                if (z >= st.p[1]) {
                    dir = -1;
                    sl_dist = st.sl_mult * cur_atr;
                    tp_dist = std::abs(static_cast<double>(view[0].c) - med) * st.tp_mult;
                    r_unit = cur_atr;
                } else if (z <= -st.p[1]) {
                    dir = 1;
                    sl_dist = st.sl_mult * cur_atr;
                    tp_dist = std::abs(static_cast<double>(view[0].c) - med) * st.tp_mult;
                    r_unit = cur_atr;
                }
            }
            break;
        }
        case 12: { // 13_Index_Velocity_Divergence
            int lb = std::max(2, st.p_int(0));
            bool use_nq = (st.inst.symbol == "ES");
            if (use_nq && !view.has_ref_nq()) return;
            if (!use_nq && !ca_has_es(view, st)) return;
            double c0_s = static_cast<double>(view[0].c);
            double ck_s = static_cast<double>(view[-(lb - 1)].c);
            double r_self = (c0_s - ck_s) / std::max(1.0, ck_s);
            double c0_o = use_nq ? static_cast<double>(view.ref_nq_bar(0).c) : static_cast<double>(ca_es_bar(view, st, 0).c);
            double ck_o = use_nq ? static_cast<double>(view.ref_nq_bar(-(lb - 1)).c) : static_cast<double>(ca_es_bar(view, st, -(lb - 1)).c);
            double r_other = (c0_o - ck_o) / std::max(1.0, ck_o);
            double diff = r_self - r_other;
            if (diff > st.p[1]) { dir = -1; set_scale_sl_tp(); }
            else if (diff < -st.p[1]) { dir = 1; set_scale_sl_tp(); }
            break;
        }
        case 13: { // 14_Volume_Weighted_Entropy
            int lb = std::max(3, st.p_int(0));
            double up_v = 0.0, dn_v = 0.0;
            for (int k = 0; k < lb; ++k) {
                if (view[-k].c >= view[-k].o) up_v += static_cast<double>(view[-k].v);
                else dn_v += static_cast<double>(view[-k].v);
            }
            double tot = up_v + dn_v;
            if (tot > 0.0) {
                double p_up = up_v / tot;
                if (p_up > st.p[1]) { dir = 1; set_atr_sl_tp(); }
                else if (p_up < (1.0 - st.p[1])) { dir = -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 14: { // 15_Volatility_Imbalance_Shock
            double rng = static_cast<double>(view[0].h) - static_cast<double>(view[0].l);
            if (rng >= st.p[0] * cur_atr && static_cast<double>(view[0].v) > st.p[1] * static_cast<double>(view[-1].v)) {
                if (static_cast<double>(view[0].c) > static_cast<double>(view[0].o) + st.p[2] * rng) {
                    dir = 1; sl_dist = st.sl_mult * rng; tp_dist = st.tp_mult * rng; r_unit = cur_atr;
                } else if (static_cast<double>(view[0].c) < static_cast<double>(view[0].o) - st.p[2] * rng) {
                    dir = -1; sl_dist = st.sl_mult * rng; tp_dist = st.tp_mult * rng; r_unit = cur_atr;
                }
            }
            break;
        }
        case 15: { // 16_TrueRange_Compression_Trap
            double tr1 = static_cast<double>(view[-1].h) - static_cast<double>(view[-1].l);
            double tr2 = static_cast<double>(view[-2].h) - static_cast<double>(view[-2].l);
            double tr3 = static_cast<double>(view[-3].h) - static_cast<double>(view[-3].l);
            double th = st.p[0] * cur_atr;
            if (tr1 < th && tr2 < th && tr3 < th) {
                if (view[0].o > view[-1].h) { dir = -1; set_scale_sl_tp(); }
                else if (view[0].o < view[-1].l) { dir = 1; set_scale_sl_tp(); }
            }
            break;
        }
        case 16: { // 17_Ornstein_Uhlenbeck_Speed
            double dev = static_cast<double>(view[0].c) - view.ema20(0);
            if (std::abs(dev) > st.p[0] * cur_atr) {
                dir = (dev > 0.0) ? -1 : 1;
                set_atr_sl_tp();
            }
            break;
        }
        case 17: { // 18_Kinetic_Energy_Conservation
            double v1 = static_cast<double>(view[0].c) - static_cast<double>(view[0].o);
            double ke1 = static_cast<double>(view[0].v) * (v1 * v1);
            double v2 = static_cast<double>(view[-1].c) - static_cast<double>(view[-1].o);
            double ke2 = static_cast<double>(view[-1].v) * (v2 * v2);
            if (ke2 > 0.0 && ke1 < st.p[0] * ke2) {
                if (view[0].h > view[-1].h) { dir = -1; set_scale_sl_tp(); }
                else if (view[0].l < view[-1].l) { dir = 1; set_scale_sl_tp(); }
            }
            break;
        }
        case 18: { // 19_POC_Migration_Drift
            int lb = std::max(2, st.p_int(0));
            double d1 = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            if (std::abs(d1) > st.p[1] * cur_atr) {
                if (d1 > 0.0 && static_cast<double>(view[0].c) <= static_cast<double>(view[0].l) + st.p[2] * cur_atr) {
                    dir = 1; set_scale_sl_tp();
                } else if (d1 < 0.0 && static_cast<double>(view[0].c) >= static_cast<double>(view[0].h) - st.p[2] * cur_atr) {
                    dir = -1; set_scale_sl_tp();
                }
            }
            break;
        }
        case 19: { // 20_Asymmetric_Wick_Absorption
            double body = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[0].o));
            double up_wick = static_cast<double>(view[0].h) - std::max(static_cast<double>(view[0].o), static_cast<double>(view[0].c));
            double dn_wick = std::min(static_cast<double>(view[0].o), static_cast<double>(view[0].c)) - static_cast<double>(view[0].l);
            if (body > 1e-6) {
                if (up_wick >= st.p[0] * body && up_wick > st.p[1] * cur_atr) {
                    dir = -1;
                    sl_dist = up_wick * st.sl_mult + 2.0 * ir;
                    tp_dist = up_wick * st.tp_mult;
                    r_unit = cur_atr;
                } else if (dn_wick >= st.p[0] * body && dn_wick > st.p[1] * cur_atr) {
                    dir = 1;
                    sl_dist = dn_wick * st.sl_mult + 2.0 * ir;
                    tp_dist = dn_wick * st.tp_mult;
                    r_unit = cur_atr;
                }
            }
            break;
        }
        default: break;
    }
}

inline void eval_group_021_040(
    const u11::CausalBarView& view, const Batch2State& st,
    double cur_atr, double scale,
    int8_t& dir, double& sl_dist, double& tp_dist, double& r_unit
) {
    const double ir = st.inst_ratio;
    auto set_atr_sl_tp = [&]() { sl_dist = st.sl_mult * cur_atr; tp_dist = st.tp_mult * cur_atr; r_unit = cur_atr; };
    auto set_scale_sl_tp = [&]() { sl_dist = st.sl_mult * scale * ir; tp_dist = st.tp_mult * scale * ir; r_unit = scale * ir; };

    switch (st.formula_id) {
        case 20: { // 21_Hilbert_Instantaneous_Phase
            int lb = std::max(4, st.p_int(0));
            int half = std::max(1, lb / 2);
            double c0 = view[0].c, cm = view[-half].c, cf = view[-(lb - 1)].c;
            double p0 = view[-1].c, pm = view[-(half + 1)].c, pf = view[-lb].c;
            double I = c0 - cf;
            double Q = c0 - 2.0 * cm + cf;
            double prev_I = p0 - pf;
            double prev_Q = p0 - 2.0 * pm + pf;
            double phase = std::atan2(Q, I);
            double prev_phase = std::atan2(prev_Q, prev_I);
            if (prev_phase < 0.0 && phase >= 0.0 && I > 0.0) { dir = 1; set_atr_sl_tp(); }
            else if (prev_phase > 0.0 && phase <= 0.0 && I < 0.0) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 21: { // 22_Kolmogorov_Sinai_Entropy
            int lb = std::max(4, st.p_int(0));
            int ones = 0;
            for (int k = 0; k < lb; ++k) {
                if (view[-k].c >= view[-(k + 1)].c) ++ones;
            }
            double p1 = static_cast<double>(ones) / static_cast<double>(lb);
            double p0 = 1.0 - p1;
            double entropy = 0.0;
            if (p1 > 0.05 && p1 < 0.95) {
                entropy = -(p1 * std::log2(p1) + p0 * std::log2(p0));
            }
            if (entropy < st.p[1] && entropy > 0.0) {
                if (p1 > st.p[2]) { dir = 1; set_atr_sl_tp(); }
                else if (p0 > st.p[2]) { dir = -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 22: { // 23_Fisher_Information_Metric
            int lb = std::max(4, st.p_int(0));
            double sum_r = 0.0, sum_sq = 0.0;
            for (int k = 0; k < lb; ++k) {
                double prev_c = std::max(1.0, static_cast<double>(view[-(k + 1)].c));
                double r = (static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c)) / prev_c;
                sum_r += r;
                sum_sq += r * r;
            }
            double mean_r = sum_r / static_cast<double>(lb);
            double var_r = (sum_sq / static_cast<double>(lb)) - (mean_r * mean_r);
            if (var_r > 1e-9) {
                double fisher = 1.0 / var_r;
                if (fisher > st.p[1] && std::abs(mean_r) > st.p[2]) {
                    dir = (mean_r > 0.0) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 23: { // 24_Kalman_Filter_Optimal_Drift
            double residual = static_cast<double>(view[0].c) - st.kalman_x;
            if (std::abs(residual) > st.p[1] * cur_atr) {
                dir = (residual > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 24: { // 25_Orderflow_Liquidity_Permeability
            int lb = std::max(2, st.p_int(0));
            double disp = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            double tot_v = 0.0;
            for (int k = 0; k < lb; ++k) tot_v += std::max(0.0, static_cast<double>(view[-k].v));
            if (tot_v > 0.0) {
                double perm = disp / std::sqrt(tot_v);
                if (perm > st.p[1] * cur_atr) { dir = 1; set_atr_sl_tp(); }
                else if (perm < -st.p[1] * cur_atr) { dir = -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 25: { // 26_Wavelet_Multiresolution_Energy
            int lb = std::max(5, st.p_int(0));
            double L = (static_cast<double>(view[0].c) + view[-1].c + view[-2].c + view[-3].c) * 0.25;
            double H = (static_cast<double>(view[0].c) - view[-1].c) - (static_cast<double>(view[-2].c) - view[-3].c);
            double ref_c = static_cast<double>(view[-(lb - 1)].c);
            double energy_ratio = std::abs(L - ref_c) / (std::abs(H) + 1.0 * ir);
            if (energy_ratio > st.p[1] && std::abs(L - ref_c) > st.p[2] * cur_atr) {
                dir = (L > ref_c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 26: { // 27_CrossAsset_Triangular_LeadLag
            if (!ca_has_nq(view, st) || !ca_has_es(view, st) || !ca_has_ym(view, st)) return;
            int lb = std::max(2, st.p_int(0));
            auto ret = [&](const u11::RawBar& a, const u11::RawBar& b) {
                return (static_cast<double>(a.c) - static_cast<double>(b.c)) / std::max(1.0, static_cast<double>(b.c));
            };
            double r_nq = ret(ca_nq_bar(view, st, 0), ca_nq_bar(view, st, -(lb - 1)));
            double r_es = ret(ca_es_bar(view, st, 0), ca_es_bar(view, st, -(lb - 1)));
            double r_ym = ret(ca_ym_bar(view, st, 0), ca_ym_bar(view, st, -(lb - 1)));
            double spread = r_es - 0.5 * (r_nq + r_ym);
            if (spread > st.p[1]) { dir = 1; set_scale_sl_tp(); }
            else if (spread < -st.p[1]) { dir = -1; set_scale_sl_tp(); }
            break;
        }
        case 27: { // 28_Hawkes_SelfExciting_Arrivals
            if (st.filter_val > st.p[2]) {
                dir = (view[0].c > view[0].o) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 28: { // 29_Asymmetric_Jump_Diffusion
            double r = static_cast<double>(view[0].c) - static_cast<double>(view[-1].c);
            if (r >= st.p[0] * cur_atr) { dir = 1; set_atr_sl_tp(); }
            else if (r <= -st.p[0] * cur_atr) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 29: { // 30_Dynamic_Volume_Sync_VBP
            double bv = (view[0].c > view[-1].c) ? static_cast<double>(view[0].v) : 0.0;
            double sv = (view[0].c < view[-1].c) ? static_cast<double>(view[0].v) : 0.0;
            double vpin = std::abs(bv - sv) / (static_cast<double>(view[0].v) + 1.0);
            if (vpin > st.p[0] && static_cast<double>(view[0].v) > st.p[1] * static_cast<double>(view[-1].v)) {
                dir = (bv > sv) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 30: { // 31_Markov_Regime_Transition_Matrix
            bool c_hvol = (static_cast<double>(view[0].h) - view[0].l) > st.p[0] * cur_atr;
            bool p_hvol = (static_cast<double>(view[-1].h) - view[-1].l) > st.p[0] * cur_atr;
            if (c_hvol && !p_hvol) {
                if (view[0].c > view[-1].h) { dir = 1; set_atr_sl_tp(); }
                else if (view[0].c < view[-1].l) { dir = -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 31: { // 32_GARCH_Volatility_Clustering
            double r = static_cast<double>(view[0].c) - static_cast<double>(view[-1].c);
            if (r < -st.p[0] * cur_atr) { dir = -1; set_atr_sl_tp(); }
            else if (r > st.p[1] * cur_atr) { dir = 1; set_atr_sl_tp(); }
            break;
        }
        case 32: { // 33_Stochastic_Resonance_Pulse
            int lb = std::max(2, st.p_int(0));
            double noise = (static_cast<double>(view[0].h) - view[0].l) - std::abs(static_cast<double>(view[0].c) - view[0].o);
            double sig_v = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c));
            if (noise > 1e-6 && (sig_v / noise) > st.p[1] && sig_v > st.p[2] * cur_atr) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 33: { // 34_Topological_Persistence_Homology
            int lb = std::max(3, st.p_int(0));
            double min_p = view[0].l, max_p = view[0].h;
            for (int k = 1; k < lb; ++k) {
                min_p = std::min(min_p, static_cast<double>(view[-k].l));
                max_p = std::max(max_p, static_cast<double>(view[-k].h));
            }
            double barcode = max_p - min_p;
            if (barcode > st.p[1] * cur_atr) {
                if (static_cast<double>(view[0].c) > min_p + st.p[2] * barcode) { dir = 1; set_atr_sl_tp(); }
                else if (static_cast<double>(view[0].c) < min_p + (1.0 - st.p[2]) * barcode) { dir = -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 34: { // 35_Information_Geometric_Curvature
            int s_lb = std::max(1, st.p_int(0) - 1);
            double v1 = static_cast<double>(view[0].c) - static_cast<double>(view[-s_lb].c);
            double v2 = static_cast<double>(view[-s_lb].c) - static_cast<double>(view[-(2 * s_lb)].c);
            double curv = v1 - v2;
            if (std::abs(curv) > st.p[1] * cur_atr) {
                dir = (curv > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 35: { // 36_Microstructure_BidAsk_Reflectance
            double dp = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[0].o));
            double dv = std::max(0.0, static_cast<double>(view[0].v)) + 1.0;
            double acoustic = (dp / std::max(1e-6, ir)) / dv;
            if (acoustic > st.p[0] && dp > st.p[1] * cur_atr) {
                dir = (view[0].c > view[0].o) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 36: { // 37_Multifractal_DFA_Spectrum
            int lb = std::max(4, st.p_int(0));
            double q2 = 0.0, q4 = 0.0;
            for (int k = 0; k < lb; ++k) {
                double dev = std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c));
                double d2 = dev * dev;
                q2 += d2;
                q4 += d2 * d2;
            }
            if (q2 > 0.0) {
                double kurt = q4 / (q2 * q2 + 1e-6);
                if (kurt > st.p[1] && (static_cast<double>(view[0].h) - view[0].l) > st.p[2] * cur_atr) {
                    dir = (view[0].c > view[-1].c) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 37: { // 38_Lyapunov_Exponent_Divergence
            int lb = std::max(3, st.p_int(0));
            double d0 = std::abs(static_cast<double>(view[-(lb - 1)].c) - static_cast<double>(view[-lb].c)) + 0.25 * ir;
            double dt = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) + 0.25 * ir;
            double ftle = (1.0 / static_cast<double>(lb)) * std::log(dt / d0);
            if (ftle > st.p[1]) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 38: { // 39_Synthetic_Portfolio_Cointegration
            if (!ca_has_nq(view, st) || !ca_has_es(view, st)) return;
            int lb = std::max(2, st.p_int(0));
            double beta = st.p[2];
            double nq0 = static_cast<double>(ca_nq_bar(view, st, 0).c);
            double syn_res = nq0 - beta * static_cast<double>(ca_es_bar(view, st, 0).c);
            double syn_mean = static_cast<double>(ca_nq_bar(view, st, -(lb - 1)).c) - beta * static_cast<double>(ca_es_bar(view, st, -(lb - 1)).c);
            double spread_err = syn_res - syn_mean;
            double scale_nq = std::clamp(nq0 / 4524.25, 0.70, 3.00);
            if (spread_err > st.p[1] * scale_nq) { dir = -1; set_scale_sl_tp(); }
            else if (spread_err < -st.p[1] * scale_nq) { dir = 1; set_scale_sl_tp(); }
            break;
        }
        case 39: { // 40_Nonlinear_Volterra_Kernel
            double x1 = static_cast<double>(view[0].c) - static_cast<double>(view[-1].c);
            double x2 = static_cast<double>(view[-1].c) - static_cast<double>(view[-2].c);
            double vy = 1.2 * x1 + 0.8 * x2 + st.p[0] * (x1 * x2);
            if (std::abs(vy) > st.p[1] * cur_atr) {
                dir = (vy > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        default: break;
    }
}

inline void eval_group_041_060(
    const u11::CausalBarView& view, const Batch2State& st,
    double cur_atr,
    int8_t& dir, double& sl_dist, double& tp_dist, double& r_unit
) {
    auto set_atr_sl_tp = [&]() { sl_dist = st.sl_mult * cur_atr; tp_dist = st.tp_mult * cur_atr; r_unit = cur_atr; };

    switch (st.formula_id) {
        case 40: // 41_Quantum_Tunneling_NQ_AllSess
        case 41: { // 41_Quantum_Tunneling_Barrier
            int lb = std::max(2, st.p_int(0));
            double d = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            double ke = 0.5 * d * d;
            double barrier = cur_atr * cur_atr;
            if (ke > st.p[2] * barrier) {
                double kappa = std::sqrt(std::max(0.01, barrier - 0.5 * ke));
                double trans_prob = std::exp(-2.0 * kappa * 0.1);
                if (trans_prob > st.p[1]) {
                    dir = (d > 0.0) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 42: { // 42_Langevin_Stochastic_Drift
            int lb1 = std::max(2, st.p_int(0));
            int lb2 = std::max(3, st.p_int(1));
            double v_drift = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb1 - 1)].c)) / static_cast<double>(lb1);
            double gamma = st.p[2];
            double grad_U = -(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb2 - 1)].c)) / (cur_atr + 1e-5);
            double force = -grad_U - gamma * v_drift;
            if (std::abs(force) > st.p[3]) { dir = (force > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 43: { // 43_EMD_Hilbert_Huang_Frequency
            double imf1 = static_cast<double>(view[0].c) - 0.5 * (static_cast<double>(view[-2].c) + view[-4].c);
            double imf1_p = static_cast<double>(view[-1].c) - 0.5 * (static_cast<double>(view[-3].c) + view[-5].c);
            double deriv = imf1 - imf1_p;
            double amp = std::sqrt(imf1 * imf1 + deriv * deriv);
            if (amp > st.p[0] * cur_atr && std::abs(deriv) > st.p[1] * cur_atr) {
                dir = (deriv > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 44: { // 44_Renyi_Entropy_OrderQ_Spectrum
            int lb = std::max(3, st.p_int(0));
            double up = 0.0, dn = 0.0;
            for (int k = 0; k < lb; ++k) {
                if (view[-k].c >= view[-(k + 1)].c) up += 1.0; else dn += 1.0;
            }
            up /= static_cast<double>(lb);
            dn /= static_cast<double>(lb);
            if (up > st.p[2] && dn > st.p[2] && std::abs(up - dn) > st.p[1]) {
                dir = (up > dn) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 45: { // 45_Fokker_Planck_Probability_Flux
            int lb1 = std::max(2, st.p_int(0));
            int lb2 = std::max(2, st.p_int(1));
            double mu = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb1 - 1)].c)) / static_cast<double>(lb1);
            double diff_D = cur_atr * 0.5;
            double ref_c = static_cast<double>(view[-(lb2 - 1)].c);
            double grad_P = (static_cast<double>(view[0].c) - ref_c) / (ref_c + 1e-5);
            double prob_flux = mu - diff_D * grad_P;
            if (std::abs(prob_flux) > st.p[2] * cur_atr) { dir = (prob_flux > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 46: { // 46_Betti_Number_Zero_Persistence
            int s_lb = std::max(2, st.p_int(0) - 1);
            double p0 = view[0].c, p1 = view[-s_lb].c, p2 = view[-(2 * s_lb + 1)].c;
            double d01 = std::abs(p0 - p1), d12 = std::abs(p1 - p2);
            if (d01 > st.p[1] * cur_atr && d12 < st.p[2] * cur_atr) {
                dir = (p0 > p1) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 47: { // 47_Hawkes_Modulated_Jump_Drift
            if (st.filter_val > st.p[1]) {
                dir = (view[0].c > view[-1].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 48: { // 48_Kuramoto_Phase_Synchronization
            if (!ca_has_nq(view, st) || !ca_has_es(view, st) || !ca_has_ym(view, st)) return;
            int lb = std::max(2, st.p_int(0));
            double r_nq = (static_cast<double>(ca_nq_bar(view, st, 0).c) - ca_nq_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_nq_atr(view, st, 0));
            double r_es = (static_cast<double>(ca_es_bar(view, st, 0).c) - ca_es_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_es_atr(view, st, 0));
            double r_ym = (static_cast<double>(ca_ym_bar(view, st, 0).c) - ca_ym_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_ym_atr(view, st, 0));
            double th = st.p[1];
            if (r_nq > th && r_es > th && r_ym > th) { dir = 1; set_atr_sl_tp(); }
            else if (r_nq < -th && r_es < -th && r_ym < -th) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 49: { // 49_Thermodynamic_Landauer_Work
            int lb = std::max(2, st.p_int(0));
            double work = 0.0;
            for (int k = 0; k < lb; ++k) {
                double dp = static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c);
                work += dp * std::max(0.0, static_cast<double>(view[-k].v));
            }
            double norm_w = work / (cur_atr * std::max(0.0, static_cast<double>(view[0].v)) + 1.0);
            if (std::abs(norm_w) > st.p[1]) { dir = (norm_w > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 50: { // 50_Fractional_Brownian_Hurst_Field
            int lb = std::max(4, st.p_int(0));
            double sq1 = 0.0, sq2 = 0.0;
            for (int k = 0; k < lb; ++k) {
                double r1 = static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c);
                sq1 += r1 * r1;
            }
            for (int k = 0; k < 2 * lb; k += 2) {
                double r2 = static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 2)].c);
                sq2 += r2 * r2;
            }
            if (sq1 > 1e-9) {
                double h_est = 0.5 * std::log2((sq2 + 1e-9) / (sq1 + 1e-9));
                if (h_est > st.p[1]) {
                    dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 51: { // 51_Koopman_DMD_Leading_Mode
            int lb = std::max(3, st.p_int(0));
            double x1 = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            double x2 = static_cast<double>(view[-1].c) - static_cast<double>(view[-lb].c);
            double koopman_a = (x1 * x2) / (x2 * x2 + 1e-5);
            if (koopman_a > st.p[1] && std::abs(x1) > st.p[2] * cur_atr) {
                dir = (x1 > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 52: { // 52_Tsallis_NonExtensive_Tail
            int lb = std::max(2, st.p_int(0));
            double ret_dev = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (std::abs(ret_dev) > st.p[1]) { dir = (ret_dev > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 53: { // 53_Lie_Algebra_Hyperbolic_Shear
            int lb = std::max(2, st.p_int(0));
            double dx = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double v_ref = std::max(0.0, static_cast<double>(view[-(lb - 1)].v));
            double dy = (static_cast<double>(view[0].v) - v_ref) / (v_ref + 1.0);
            double shear = dx * dx - dy * dy;
            if (shear > st.p[1] && std::abs(dx) > st.p[2]) { dir = (dx > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 54: { // 54_Graph_Laplacian_Fiedler_Cut
            int lb1 = std::max(2, st.p_int(0));
            int lb2 = std::max(lb1 + 1, st.p_int(1));
            double p_mid = 0.5 * (static_cast<double>(view[-(lb1 - 1)].c) + static_cast<double>(view[-(lb2 - 1)].c));
            double fiedler = (static_cast<double>(view[0].c) - p_mid) / cur_atr;
            if (std::abs(fiedler) > st.p[2]) { dir = (fiedler > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 55: { // 55_CWT_Morlet_Ridge_Tracking
            int lb = std::max(4, st.p_int(0));
            double real_m = 0.0, imag_m = 0.0;
            double half = 0.5 * static_cast<double>(lb);
            double sig_w = std::max(1.0, static_cast<double>(lb) / 4.0);
            for (int k = 1; k <= lb; ++k) {
                double z = (static_cast<double>(k) - half) / sig_w;
                double w = std::exp(-0.5 * z * z);
                double ph = 2.0 * kPi * static_cast<double>(k) / half;
                double c_val = static_cast<double>(view[-(k - 1)].c);
                real_m += c_val * w * std::cos(ph);
                imag_m += c_val * w * std::sin(ph);
            }
            double ridge = (real_m * real_m + imag_m * imag_m) / (cur_atr * cur_atr + 1.0);
            if (ridge > st.p[1]) { dir = (real_m > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 56: { // 56_Wasserstein_EarthMover_Flux
            int lb = std::max(2, st.p_int(0));
            double m1 = 0.0, m2 = 0.0;
            for (int k = 0; k < lb; ++k) {
                m1 += static_cast<double>(view[-k].c);
                m2 += static_cast<double>(view[-(lb + k)].c);
            }
            m1 /= static_cast<double>(lb);
            m2 /= static_cast<double>(lb);
            if (std::abs(m1 - m2) > st.p[1] * cur_atr) { dir = (m1 > m2) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 57: { // 57_Volterra_Memory_Dissipation
            int lb = std::max(3, st.p_int(0));
            double mem = 0.0;
            for (int k = 1; k <= lb; ++k) {
                double kernel = std::exp(-st.p[1] * static_cast<double>(k));
                mem += kernel * (static_cast<double>(view[-(k - 1)].c) - static_cast<double>(view[-k].c));
            }
            if (std::abs(mem) > st.p[2] * cur_atr) { dir = (mem > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 58: { // 58_Poincare_Map_Tangent_Divergence
            int lb = std::max(2, st.p_int(0));
            double xn = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            double xnp = static_cast<double>(view[-lb].c) - static_cast<double>(view[-(2 * lb - 1)].c);
            if (std::abs(xnp) > 0.1 * cur_atr) {
                double slope = std::abs(xn / xnp);
                if (slope > st.p[1] && std::abs(xn) > st.p[2] * cur_atr) {
                    dir = (xn > 0.0) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 59: { // 59_Girsanov_Measure_Change_Shift
            int lb = std::max(2, st.p_int(0));
            double theta = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / (cur_atr + 1e-5);
            if (std::abs(theta) > st.p[1]) { dir = (theta > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 60: { // 60_Quantum_Harmonic_Displacement
            int lb = std::max(2, st.p_int(0));
            double disp = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (std::abs(disp) > st.p[1]) { dir = (disp > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        default: break;
    }
}

inline void eval_group_061_080(
    const u11::CausalBarView& view, const Batch2State& st,
    double cur_atr,
    int8_t& dir, double& sl_dist, double& tp_dist, double& r_unit
) {
    auto set_atr_sl_tp = [&]() { sl_dist = st.sl_mult * cur_atr; tp_dist = st.tp_mult * cur_atr; r_unit = cur_atr; };

    switch (st.formula_id) {
        case 61: { // 61_Relativistic_Lorentz_Flow
            double c_mkt = st.p[0] * cur_atr;
            double u1 = (static_cast<double>(view[0].c) - static_cast<double>(view[-2].c)) * 0.5;
            double u2 = (static_cast<double>(view[-2].c) - static_cast<double>(view[-4].c)) * 0.5;
            double w = (u1 + u2) / (1.0 + (u1 * u2) / (c_mkt * c_mkt + 1e-5));
            double beta = std::abs(w) / std::max(1e-6, c_mkt);
            if (beta > st.p[2] && beta < 0.98) {
                double gamma = 1.0 / std::sqrt(1.0 - beta * beta);
                if (gamma > st.p[1]) { dir = (w > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 62: { // 62_SpinGlass_Frustration_Relaxation
            int lb = std::max(3, st.p_int(0));
            double net_spin = 0.0;
            for (int k = 0; k < lb; ++k) {
                net_spin += (view[-k].c >= view[-(k + 1)].c) ? 1.0 : -1.0;
            }
            double mag = net_spin / static_cast<double>(lb);
            if (std::abs(mag) > st.p[1]) { dir = (mag > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 63: { // 63_Burgers_Shockwave_Singularity
            int s = std::max(1, st.p_int(0) - 1);
            double uf = static_cast<double>(view[0].c) - static_cast<double>(view[-s].c);
            double ub = static_cast<double>(view[-s].c) - static_cast<double>(view[-(2 * s + 1)].c);
            double steep = (uf - ub) / cur_atr;
            if (std::abs(steep) > st.p[1]) { dir = (steep > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 64: // 64_KPZ_Surface_Growth_Roughness
        case 65: { // 64_KPZ_Surface_Growth_NQ_NY
            int lb = std::max(3, st.p_int(0));
            double h_mean = 0.0;
            for (int k = 0; k < lb; ++k) h_mean += static_cast<double>(view[-k].c);
            h_mean /= static_cast<double>(lb);
            double r_sq = 0.0;
            for (int k = 0; k < lb; ++k) {
                double d = static_cast<double>(view[-k].c) - h_mean;
                r_sq += d * d;
            }
            double w_width = std::sqrt(r_sq / static_cast<double>(lb));
            if (w_width > st.p[1] * cur_atr) {
                dir = (static_cast<double>(view[0].c) > h_mean) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 66: { // 65_SLE_Conformal_Trace
            int lb = std::max(2, st.p_int(0));
            double dw = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double kappa = dw * dw;
            if (kappa > st.p[1] && kappa < st.p[2]) {
                dir = (dw > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 67: { // 66_Sandpile_Avalanche_Cascade
            if (std::abs(st.filter_val) > st.p[2]) {
                dir = (st.filter_val > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 68: // 67_Amari_Dual_Connection_YM
        case 69: { // 67_Amari_Dual_Connection_Flux
            int lb1 = std::max(2, st.p_int(0));
            int lb2 = std::max(lb1 + 1, st.p_int(1));
            double ge = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb1 - 1)].c)) / cur_atr;
            double gm = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb2 - 1)].c)) / cur_atr;
            double flux = 1.5 * ge - 0.5 * gm;
            if (std::abs(flux) > st.p[2]) { dir = (flux > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 70: { // 68_NonHermitian_Exceptional_Point
            int s = std::max(2, st.p_int(0) - 1);
            double dw = (static_cast<double>(view[0].c) - static_cast<double>(view[-s].c)) / cur_atr;
            double dg = (static_cast<double>(view[-s].c) - static_cast<double>(view[-(2 * s + 1)].c)) / cur_atr;
            double ep = std::abs(dw * dw - dg * dg);
            if (ep < st.p[1] && std::abs(dw) > st.p[2]) { dir = (dw > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 71: { // 69_Brusselator_Limit_Cycle
            int lb = std::max(2, st.p_int(0));
            double x = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double v_ref = std::max(0.0, static_cast<double>(view[-(lb - 1)].v));
            double y = (static_cast<double>(view[0].v) - v_ref) / (v_ref + 1.0);
            double rad = std::sqrt(x * x + y * y);
            if (rad > st.p[1] && std::abs(x) > st.p[2]) { dir = (x > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 72: { // 70_Fractional_Poisson_Burst
            int lb = std::max(3, st.p_int(0));
            int bursts = 0;
            for (int k = 0; k < lb; ++k) {
                if (std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c)) > st.p[2] * cur_atr) ++bursts;
            }
            if (bursts >= st.p_int(1)) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 73: { // 71_DeSitter_Cosmological_Horizon
            int lb = std::max(2, st.p_int(0));
            double a_now = view.atr14(0);
            double a_old = view.atr14(-(lb - 1));
            double hubble = (a_now - a_old) / (a_old + 1e-5);
            double disp = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (hubble > st.p[1] && std::abs(disp) > st.p[2]) { dir = (disp > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 74: { // 72_Feigenbaum_Chaos_Cascade
            int lb = std::max(2, st.p_int(0));
            double r_chaos = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / (cur_atr + 1e-5);
            if (r_chaos > st.p[1] && r_chaos < st.p[2]) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 75: { // 73_YangMills_Gauge_Curvature
            if (!ca_has_nq(view, st) || !ca_has_es(view, st)) return;
            int lb = std::max(2, st.p_int(0));
            double a_nq = (static_cast<double>(ca_nq_bar(view, st, 0).c) - ca_nq_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_nq_atr(view, st, 0));
            double a_es = (static_cast<double>(ca_es_bar(view, st, 0).c) - ca_es_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_es_atr(view, st, 0));
            double curl = a_nq - a_es;
            if (std::abs(curl) > st.p[1] && std::abs(a_nq) > st.p[2]) {
                dir = (a_nq > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 76: { // 74_NavierStokes_Enstrophy_Jet
            if (st.filter_val > st.p[1] * cur_atr * cur_atr) {
                double disp = static_cast<double>(view[0].c) - static_cast<double>(view[-4].c);
                if (std::abs(disp) > st.p[2] * cur_atr) {
                    dir = (disp > 0.0) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 77: { // 75_KdV_Soliton_Envelope
            int lb = std::max(3, st.p_int(0));
            int half = std::max(1, (lb - 1) / 2);
            double sech_h = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double sech_c = (static_cast<double>(view[0].c) - 2.0 * static_cast<double>(view[-half].c) + static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (std::abs(sech_h) > st.p[1] && std::abs(sech_c) < st.p[2]) {
                dir = (sech_h > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 78: { // 76_GinzburgLandau_Supercurrent
            int lb1 = std::max(2, st.p_int(0));
            int lb2 = std::max(2, st.p_int(1));
            double psi = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb1 - 1)].c)) / cur_atr;
            double grad = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb2 - 1)].c)) / cur_atr;
            double j_sc = psi * grad;
            if (std::abs(j_sc) > st.p[2]) { dir = (j_sc > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 79: { // 77_Kolmogorov_Turbulence_Cascade
            int lb = std::max(2, st.p_int(0));
            double du = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double du3 = du * du * du;
            if (std::abs(du3) > st.p[1]) { dir = (du3 > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 80: { // 78_Penrose_Ergosphere_Extraction
            int lb = std::max(2, st.p_int(0));
            double ergo = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double esc = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            if (std::abs(ergo) > st.p[1] && std::abs(esc) > st.p[2] && (ergo * esc > 0.0)) {
                dir = (ergo > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 81: { // 79_Riemann_Zeta_Resonant_Spacing
            int lb = std::max(4, st.p_int(0));
            double s1 = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-2].c));
            double s2 = std::abs(static_cast<double>(view[-2].c) - static_cast<double>(view[-(lb - 1)].c));
            double ratio = s1 / (s2 + 1e-5);
            double disp = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            if (ratio > st.p[1] && std::abs(disp) > st.p[2] * cur_atr) {
                dir = (disp > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 82: { // 80_CFT_Virasoro_Central_Charge
            int lb = std::max(4, st.p_int(0));
            double uv = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-2].c));
            double ir_s = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c));
            double c_ratio = uv / (ir_s + 1e-5);
            if (c_ratio > st.p[1] && ir_s > st.p[2] * cur_atr) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        default: break;
    }
}

inline void eval_group_081_100(
    const u11::CausalBarView& view, const Batch2State& st,
    double cur_atr,
    int8_t& dir, double& sl_dist, double& tp_dist, double& r_unit
) {
    auto set_atr_sl_tp = [&]() { sl_dist = st.sl_mult * cur_atr; tp_dist = st.tp_mult * cur_atr; r_unit = cur_atr; };

    switch (st.formula_id) {
        case 83: { // 81_Casimir_Vacuum_Fluctuation
            int lb = std::max(2, st.p_int(0));
            double gap = (static_cast<double>(view[0].h) - static_cast<double>(view[0].l)) / (cur_atr + 1e-5);
            if (gap < st.p[1]) {
                double disp = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
                double d = gap + 0.05;
                double casimir = 1.0 / (d * d * d * d);
                if (casimir > st.p[2] && std::abs(disp) > st.p[3]) {
                    dir = (disp > 0.0) ? 1 : -1;
                    set_atr_sl_tp();
                }
            }
            break;
        }
        case 84: { // 82_Moyal_Bracket_Star_Product
            double dx1 = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            double v1 = std::max(0.0, static_cast<double>(view[-1].v));
            double dv1 = (static_cast<double>(view[0].v) - v1) / (v1 + 1.0);
            double dx2 = (static_cast<double>(view[-1].c) - static_cast<double>(view[-2].c)) / cur_atr;
            double v2 = std::max(0.0, static_cast<double>(view[-2].v));
            double dv2 = (static_cast<double>(view[-1].v) - v2) / (v2 + 1.0);
            double moyal = dx1 * dv2 - dx2 * dv1;
            if (std::abs(moyal) > st.p[0]) { dir = (moyal > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 85: { // 83_Ricci_Flow_Metric_Singularity
            int lb = std::max(4, st.p_int(0));
            int half = std::max(2, (lb - 1) / 2);
            double r_curv = (static_cast<double>(view[0].c) - 2.0 * static_cast<double>(view[-half].c) + static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double shrink = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (std::abs(r_curv) > st.p[1] && std::abs(shrink) > st.p[2]) {
                dir = (shrink > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 86: { // 84_Berry_Geometric_Phase_Flux
            int lb = std::max(2, st.p_int(0));
            double p_disp = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double v_disp = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            double berry = p_disp * v_disp;
            if (std::abs(berry) > st.p[1]) { dir = (berry > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 87: { // 85_GinzburgLandau_Vortex_Melting
            int lb = std::max(2, st.p_int(0));
            double disp = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            if (st.filter_val > st.p[2] && std::abs(disp) > st.p[3] * cur_atr) {
                dir = (disp > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 88: { // 86_AdSCFT_Holographic_Bulk_Current
            int lb_ir = std::max(4, st.p_int(0));
            int lb_uv = std::max(2, st.p_int(1));
            double uv = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb_uv - 1)].c)) / cur_atr;
            double ir_d = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb_ir - 1)].c)) / cur_atr;
            double bulk = uv * std::sqrt(std::abs(ir_d) + 0.1);
            if (std::abs(bulk) > st.p[2]) { dir = (bulk > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 89: { // 87_Majorana_ZeroMode_Braiding
            int s = std::max(2, st.p_int(0) - 1);
            double p1 = (view[0].c > view[-s].c) ? 1.0 : -1.0;
            double p2 = (view[-s].c > view[-(2 * s + 1)].c) ? 1.0 : -1.0;
            double p3 = (view[-(2 * s + 1)].c > view[-(3 * s + 2)].c) ? 1.0 : -1.0;
            if (p1 == p2 && p2 == p3) {
                double braid = (static_cast<double>(view[0].c) - static_cast<double>(view[-(3 * s + 2)].c)) / cur_atr;
                if (std::abs(braid) > st.p[1]) { dir = (braid > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 90: { // 88_NonArchimedean_PAdic_Valuation
            int lb3 = std::max(2, st.p_int(0));
            int lb9 = std::max(lb3 + 1, st.p_int(1));
            double d3 = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb3 - 1)].c));
            double d9 = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb9 - 1)].c));
            double ratio = d9 / (d3 + 1e-5);
            if (ratio > st.p[2] && d9 > st.p[3] * cur_atr) {
                dir = (view[0].c > view[-(lb9 - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 91: { // 89_FPUT_Recurrence_Breakdown
            int lb = std::max(4, st.p_int(0));
            int half = std::max(2, (lb - 1) / 2);
            double m1 = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            double m2 = static_cast<double>(view[0].c) - 2.0 * static_cast<double>(view[-half].c) + static_cast<double>(view[-(lb - 1)].c);
            double th_e = m1 * m1 + m2 * m2;
            if (th_e > st.p[1] * cur_atr * cur_atr && std::abs(m1) > st.p[2] * cur_atr) {
                dir = (m1 > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 92: { // 90_Quantum_Hall_Chiral_Edge
            int lb = std::max(2, st.p_int(0));
            double local_d = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double r_cross = local_d;
            if (st.inst.symbol != "NQ") {
                if (!view.has_ref_nq()) return;
                r_cross = (static_cast<double>(view.ref_nq_bar(0).c) - view.ref_nq_bar(-(lb - 1)).c) / std::max(1e-6, view.ref_nq_atr14(0));
            }
            if (std::abs(r_cross) > st.p[1] && std::abs(local_d) > st.p[1] && (r_cross * local_d > 0.0)) {
                dir = (local_d > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 93: { // 91_Fractional_QBM_Entanglement
            int lb = std::max(3, st.p_int(0));
            double hp = 0.0;
            for (int k = 0; k < lb; ++k) {
                hp += std::abs(static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c));
            }
            double net_d = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-lb].c));
            double hr = net_d / (hp + 1e-5);
            if (hr > st.p[1] && net_d > st.p[2] * cur_atr) {
                dir = (view[0].c > view[-lb].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 94: { // 92_GutenbergRichter_SOC_Avalanche
            int lb = std::max(2, st.p_int(0));
            double slip = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            if (std::abs(slip) > st.p[1] && std::abs(static_cast<double>(view[0].c) - view[-(lb - 1)].c) > st.p[2] * cur_atr) {
                dir = (slip > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 95: { // 93_RayleighBenard_Convection_Plume
            if (std::abs(st.filter_val) > st.p[2]) {
                dir = (st.filter_val > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 96: // 94_NLS_Peregrine_Rogue_YM_NY
        case 97: { // 94_NLS_Peregrine_Rogue_Wave
            int lb = std::max(2, st.p_int(0));
            double amp = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double bg = (static_cast<double>(view[-(lb - 1)].h) - view[-(lb - 1)].l) / cur_atr;
            if (amp > st.p[1] * bg && amp > st.p[2]) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 98: { // 95_Wigner_Surmise_Spectral_Crossing
            int lb = std::max(4, st.p_int(0));
            double e1 = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-2].c));
            double e2 = std::abs(static_cast<double>(view[-2].c) - static_cast<double>(view[-(lb - 1)].c));
            double repul = std::abs(e1 - e2) / cur_atr;
            double disp = static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c);
            if (repul < st.p[1] && std::abs(disp) > st.p[2] * cur_atr) {
                dir = (disp > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 99: { // 96_KAM_Torus_Resonance_Overlap
            int lb = std::max(5, st.p_int(0));
            double w1 = (static_cast<double>(view[0].c) - static_cast<double>(view[-3].c)) / cur_atr;
            double w2 = (static_cast<double>(view[-3].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double chirikov = std::abs(w1 * w2);
            if (chirikov > st.p[1] && (w1 * w2 > 0.0)) {
                dir = (w1 > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 100: // 97_Chern_Number_Topological_Band
        case 101: { // 97_Chern_Number_Topological_NQ
            int lb = std::max(4, st.p_int(0));
            double kx = (static_cast<double>(view[0].c) - static_cast<double>(view[-2].c)) / cur_atr;
            double ky = (static_cast<double>(view[-2].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double chern = kx * ky;
            if (std::abs(chern) > st.p[1] && std::abs(kx) > st.p[2]) {
                dir = (kx > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 102: { // 98_RankineHugoniot_Shockwave_Jump
            int lb = std::max(3, st.p_int(0));
            double u_pre = std::abs(static_cast<double>(view[-1].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double u_post = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            if (u_post > st.p[1] * u_pre && u_post > st.p[2]) {
                dir = (view[0].c > view[-1].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 103: { // 99_Jarzynski_NonEquilibrium_Work
            int lb = std::max(2, st.p_int(0));
            double work = 0.0;
            for (int k = 0; k < lb; ++k) {
                double dp = static_cast<double>(view[-k].c) - static_cast<double>(view[-(k + 1)].c);
                work += dp * std::max(0.0, static_cast<double>(view[-k].v));
            }
            double norm_w = work / (cur_atr * std::max(0.0, static_cast<double>(view[0].v)) + 1.0);
            if (std::abs(norm_w) > st.p[1]) { dir = (norm_w > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 104: { // 100_Quantum_SpinHall_Helical_Edge
            int lb = std::max(6, st.p_int(0));
            double s_up = (static_cast<double>(view[0].c) - static_cast<double>(view[-4].c)) / cur_atr;
            double s_dn = (static_cast<double>(view[-4].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (s_up * s_dn > 0.0 && std::abs(s_up) > st.p[1] && std::abs(s_dn) > st.p[1]) {
                dir = (s_up > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        default: break;
    }
}

inline void eval_group_101_120(
    const u11::CausalBarView& view, const Batch2State& st,
    double cur_atr,
    int8_t& dir, double& sl_dist, double& tp_dist, double& r_unit
) {
    auto set_atr_sl_tp = [&]() { sl_dist = st.sl_mult * cur_atr; tp_dist = st.tp_mult * cur_atr; r_unit = cur_atr; };

    switch (st.formula_id) {
        case 105: { // 101_CrossAsset_Volatility_Tensor
            if (!ca_has_es(view, st) || !ca_has_ym(view, st)) return;
            int lb = std::max(2, st.p_int(0));
            double es_m = (static_cast<double>(ca_es_bar(view, st, 0).c) - ca_es_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_es_atr(view, st, 0));
            double ym_m = (static_cast<double>(ca_ym_bar(view, st, 0).c) - ca_ym_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_ym_atr(view, st, 0));
            double sf_m = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            if (es_m > st.p[1] && ym_m > st.p[1] && sf_m < st.p[2]) { dir = 1; set_atr_sl_tp(); }
            else if (es_m < -st.p[1] && ym_m < -st.p[1] && sf_m > -st.p[2]) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 106: { // 102_Quantum_Potential_Well_Tunneling
            int lb = std::max(3, st.p_int(0));
            double mid = 0.5 * (static_cast<double>(view[0].c) + static_cast<double>(view[-(lb - 1)].c));
            double dev = (static_cast<double>(view[0].c) - mid) / cur_atr;
            double V = 0.5 * dev * dev;
            double kin = (static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) / cur_atr;
            double E = 0.5 * kin * kin;
            if (E > V && dev > st.p[1]) { dir = 1; set_atr_sl_tp(); }
            else if (E > V && dev < -st.p[1]) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 107: { // 103_OrderFlow_Kinetic_Energy_Decel
            int lb_v = std::max(2, st.p_int(0));
            double v1 = static_cast<double>(view[0].c) - static_cast<double>(view[-2].c);
            double v2 = static_cast<double>(view[-2].c) - static_cast<double>(view[-4].c);
            double a = v1 - v2;
            double vol_ratio = std::max(0.0, static_cast<double>(view[0].v)) / (std::max(0.0, static_cast<double>(view[-(lb_v - 1)].v)) + 1e-4);
            if (std::abs(a) < st.p[1] * cur_atr && std::abs(v1) > st.p[2] * cur_atr && vol_ratio > st.p[3]) {
                dir = (v1 > 0.0) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 108: { // 104_Localized_Hurst_Wavelet_Trans
            int lb = std::max(4, st.p_int(0));
            double mean = 0.0;
            for (int k = 0; k < lb; ++k) mean += static_cast<double>(view[-k].c);
            mean /= static_cast<double>(lb);
            double dma = 0.0;
            for (int k = 0; k < lb; ++k) dma += std::abs(static_cast<double>(view[-k].c) - mean);
            dma /= static_cast<double>(lb);
            double hurst = (dma > 1e-4) ? (std::log(dma / cur_atr) / std::log(static_cast<double>(lb)) + 0.5) : 0.5;
            if (hurst > st.p[1]) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 109: { // 105_Thermodynamic_Gibbs_Phase_Trans
            int lb = std::max(2, st.p_int(0));
            double v_now = std::max(0.0, static_cast<double>(view[0].v));
            double v_old = std::max(0.0, static_cast<double>(view[-(lb - 1)].v));
            double dH = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) * v_now;
            double dS = std::log(v_now / (v_old + 1e-4) + 1.0);
            double dG = dH - cur_atr * dS * st.p[1];
            if (dG > st.p[2] * cur_atr) { dir = 1; set_atr_sl_tp(); }
            else if (dG < -st.p[2] * cur_atr) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 110: { // 106_EMD_Hilbert_Huang_Phase_Lock
            double imf1 = static_cast<double>(view[0].c) - 0.5 * (static_cast<double>(view[-1].c) + view[-2].c);
            double imf2 = 0.5 * (static_cast<double>(view[-1].c) + view[-2].c) - 0.5 * (static_cast<double>(view[-7].c) + view[-8].c);
            if (imf1 > st.p[0] * cur_atr && imf2 > st.p[1] * cur_atr) { dir = 1; set_atr_sl_tp(); }
            else if (imf1 < -st.p[0] * cur_atr && imf2 < -st.p[1] * cur_atr) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 111: { // 107_Kalman_Innovation_Vector_Surge
            if (st.filter_val > st.p[2] * cur_atr) { dir = 1; set_atr_sl_tp(); }
            else if (st.filter_val < -st.p[2] * cur_atr) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 112: { // 108_Ornstein_Uhlenbeck_Jump_Escape
            double diff = static_cast<double>(view[0].c) - st.filter_val;
            double rng = static_cast<double>(view[0].h) - static_cast<double>(view[0].l);
            if (rng > st.p[1] * cur_atr) { dir = (diff > 0.0) ? 1 : -1; set_atr_sl_tp(); }
            break;
        }
        case 113: { // 109_Spectral_Wavelet_Coherence_Lead
            int lb = std::max(2, st.p_int(0));
            bool use_nq = (st.inst.symbol == "ES");
            if (use_nq && !view.has_ref_nq()) return;
            if (!use_nq && !ca_has_es(view, st)) return;
            double norm_self = (static_cast<double>(view[0].c) - static_cast<double>(view[-(lb - 1)].c)) / cur_atr;
            double norm_ref = use_nq
                ? ((static_cast<double>(view.ref_nq_bar(0).c) - view.ref_nq_bar(-(lb - 1)).c) / std::max(1e-6, view.ref_nq_atr14(0)))
                : ((static_cast<double>(ca_es_bar(view, st, 0).c) - ca_es_bar(view, st, -(lb - 1)).c) / std::max(1e-6, ca_es_atr(view, st, 0)));
            if (norm_ref > st.p[1] && norm_self < st.p[2]) { dir = 1; set_atr_sl_tp(); }
            else if (norm_ref < -st.p[1] && norm_self > -st.p[2]) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        case 114: { // 110_Tsallis_NonExtensive_Entropy_Drop
            int lb = std::max(3, st.p_int(0));
            int up_cnt = 0;
            for (int k = 0; k < lb; ++k) {
                if (view[-k].c > view[-k].o) ++up_cnt;
            }
            double p_up = static_cast<double>(up_cnt) / static_cast<double>(lb);
            double p_dn = 1.0 - p_up;
            double q = st.p[1];
            double tsallis = (std::abs(q - 1.0) > 1e-6)
                ? ((1.0 - (std::pow(p_up, q) + std::pow(p_dn, q))) / (q - 1.0))
                : 1.0;
            if (tsallis < st.p[2]) {
                if (p_up > st.p[3]) { dir = 1; set_atr_sl_tp(); }
                else if (p_dn > st.p[3]) { dir = -1; set_atr_sl_tp(); }
            }
            break;
        }
        case 115: { // 111_Stochastic_FastScale_Vol_Asymmetry
            double fast_v = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-1].c));
            if (fast_v > st.p[0] * cur_atr) {
                dir = (view[0].c > view[0].o) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 116: { // 112_Microstructural_Block_Absorption
            int lb = std::max(2, st.p_int(0));
            double rng = static_cast<double>(view[0].h) - static_cast<double>(view[0].l);
            double v_dens = std::max(0.0, static_cast<double>(view[0].v)) / (rng + 1e-4);
            double avg_dens = std::max(0.0, static_cast<double>(view[-(lb - 1)].v)) / cur_atr;
            if (v_dens > st.p[1] * avg_dens && rng < st.p[2] * cur_atr) {
                dir = (static_cast<double>(view[0].c) > 0.5 * (static_cast<double>(view[0].h) + view[0].l)) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 117: { // 113_Hawkes_SelfExciting_Cluster_Burst
            if (st.hawkes_pos > st.p[1] && st.hawkes_pos > st.p[2] * st.hawkes_neg) {
                dir = 1; set_atr_sl_tp();
            } else if (st.hawkes_neg > st.p[1] && st.hawkes_neg > st.p[2] * st.hawkes_pos) {
                dir = -1; set_atr_sl_tp();
            }
            break;
        }
        case 118: { // 114_Lyapunov_Exponent_Chaos_Horizon
            int lb = std::max(2, st.p_int(0));
            double d0 = std::abs(static_cast<double>(view[-(lb - 1)].c) - static_cast<double>(view[-lb].c)) + 1e-4;
            double d1 = std::abs(static_cast<double>(view[0].c) - static_cast<double>(view[-1].c)) + 1e-4;
            double lyap = std::log(d1 / d0) / std::max(1.0, static_cast<double>(lb - 1));
            if (lyap > st.p[1] && d1 > st.p[2] * cur_atr) {
                dir = (view[0].c > view[-(lb - 1)].c) ? 1 : -1;
                set_atr_sl_tp();
            }
            break;
        }
        case 119: { // 115_Fractional_Brownian_Motion_Memory
            double fdiff = static_cast<double>(view[0].c) - 0.4 * view[-1].c - 0.12 * view[-2].c - 0.064 * view[-3].c;
            double fprev = static_cast<double>(view[-1].c) - 0.4 * view[-2].c - 0.12 * view[-3].c - 0.064 * view[-4].c;
            double th = st.p[0] * cur_atr;
            if (fdiff > th && fprev <= th) { dir = 1; set_atr_sl_tp(); }
            else if (fdiff < -th && fprev >= -th) { dir = -1; set_atr_sl_tp(); }
            break;
        }
        default: break;
    }
}

inline u11::PluginSignal batch2_on_bar(const u11::CausalBarView& view, void* state_ptr) {
    u11::PluginSignal sig{};
    auto* st = static_cast<Batch2State*>(state_ptr);
    if (!st) return sig;
    if (view.bar_index() < static_cast<size_t>(st->req_bars)) return sig;

    double cur_atr = view.atr14(0);
    if (cur_atr <= 1e-6) return sig;

    // Update stateful recursive filters causally on completed bars
    update_stateful_filters(view, *st, cur_atr);

    if (view.in_position) return sig;
    int8_t sid = view.session_id();
    if (sid < 0 || sid > 3) return sig;
    if ((st->session_mask & (1 << sid)) == 0) return sig;

    double scale = std::clamp(static_cast<double>(view[0].c) / st->inst.baseline_index, 0.70, 3.00);
    int8_t dir = 0;
    double sl_dist = 0.0;
    double tp_dist = 0.0;
    double r_unit = cur_atr;

    if (st->formula_id < 20) {
        eval_group_001_020(view, *st, cur_atr, scale, dir, sl_dist, tp_dist, r_unit);
    } else if (st->formula_id < 40) {
        eval_group_021_040(view, *st, cur_atr, scale, dir, sl_dist, tp_dist, r_unit);
    } else if (st->formula_id <= 60) {
        eval_group_041_060(view, *st, cur_atr, dir, sl_dist, tp_dist, r_unit);
    } else if (st->formula_id <= 82) {
        eval_group_061_080(view, *st, cur_atr, dir, sl_dist, tp_dist, r_unit);
    } else if (st->formula_id <= 104) {
        eval_group_081_100(view, *st, cur_atr, dir, sl_dist, tp_dist, r_unit);
    } else {
        eval_group_101_120(view, *st, cur_atr, dir, sl_dist, tp_dist, r_unit);
    }

    if (dir == 0) return sig;

    double min_pts = st->inst.tick_size * 4.0;
    sig.direction = dir;
    sig.order_type = 0;
    sig.sl_pts = std::max(min_pts, sl_dist);
    sig.tp_pts = std::max(min_pts, tp_dist);
    sig.max_hold_bars = st->max_hold;
    sig.cooldown_bars = 1;
    if (st->ratch_trig > 0.0) {
        sig.ratchet_trig_pts = st->ratch_trig * r_unit;
        sig.ratchet_trail_pts = std::max(st->inst.tick_size, st->ratch_trail * r_unit);
    }
    return sig;
}

template <int FormulaID>
inline void batch2_init_state(Batch2State& st, const std::vector<double>& pvals, const u11::InstrumentSpec* spec) {
    st.init(FormulaID, pvals, spec);
}

template <int FormulaID>
inline void register_one_batch2(std::vector<s546::RegisteredStrategy>& out) {
    const auto& m = kBatch2Meta[FormulaID];
    s546::RegisteredStrategy reg;
    reg.uid = m.uid;
    reg.display_name = m.display_name;
    reg.super_family = "02_Physics_Quant_Multiverse_001_240";
    reg.source_note = m.source_note;
    reg.fidelity = m.fidelity;
    reg.needs_cross_asset = m.needs_cross_asset;
    reg.session_mask = m.session_mask;
    reg.schema = make_batch2_schema(FormulaID);
    reg.factory = s546::make_factory<Batch2State>(m.uid, &batch2_on_bar, &batch2_init_state<FormulaID>);
    out.push_back(std::move(reg));
}

template <size_t... Is>
inline void register_batch2_seq(std::vector<s546::RegisteredStrategy>& out, std::index_sequence<Is...>) {
    (register_one_batch2<static_cast<int>(Is)>(out), ...);
}

} // namespace batch2_detail

inline void register_batch2(std::vector<s546::RegisteredStrategy>& out) {
    out.reserve(out.size() + 120);
    batch2_detail::register_batch2_seq(out, std::make_index_sequence<120>{});
}

} // namespace s546
