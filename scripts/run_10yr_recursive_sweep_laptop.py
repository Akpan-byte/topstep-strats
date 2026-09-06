import sys, time, json, os
from pathlib import Path
import numpy as np
import pandas as pd
import multiprocessing as mp
from datetime import datetime
import numba

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT))

from topstep_strats.metrics import calculate_metrics

RESULTS_DIR = _ROOT / "sweep_results"
RESULTS_DIR.mkdir(parents=True, exist_ok=True)
STREAM_FILE = RESULTS_DIR / "stream_results.jsonl"
LEADERBOARD_FILE = RESULTS_DIR / "LIVE_LEADERBOARD.md"

PT_VALUES = {"ES": 50.0, "NQ": 20.0, "YM": 5.0}
TICK_SIZES = {"ES": 0.25, "NQ": 0.25, "YM": 1.0}

# ---------------------------------------------------------------------------
# NUMBA JIT CORE SIGNAL GENERATORS & TRADE SIMULATOR
# ---------------------------------------------------------------------------

@numba.njit(fastmath=True)
def simulate_fast(opens, highs, lows, closes, entries, sl_pts, tp_pts, max_hold_bars=120):
    """
    Simulates trade execution with strict lookahead-free ordering.
    Signal at bar i entry filled at opens[i].
    SL and TP evaluated on highs[i], lows[i].
    If both touch, Stop-Loss is prioritized first (conservative / worst-case).
    """
    n = len(closes)
    max_trades = 150000
    entry_indices = np.empty(max_trades, dtype=np.int32)
    exit_indices = np.empty(max_trades, dtype=np.int32)
    directions = np.empty(max_trades, dtype=np.int8)
    entry_prices = np.empty(max_trades, dtype=np.float64)
    exit_prices = np.empty(max_trades, dtype=np.float64)
    pnls = np.empty(max_trades, dtype=np.float64)
    reasons = np.empty(max_trades, dtype=np.int8) # 1: SL, 2: TP, 3: SL_CONSERVATIVE, 4: TIME
    
    trade_count = 0
    in_pos = False
    entry_idx = 0
    entry_p = 0.0
    dir_val = 0
    sl_p = 0.0
    tp_p = 0.0
    
    for i in range(1, n - 1):
        if not in_pos:
            sig = entries[i]
            if sig != 0:
                in_pos = True
                entry_idx = i
                dir_val = sig
                entry_p = opens[i]
                if dir_val == 1:
                    sl_p = entry_p - sl_pts
                    tp_p = entry_p + tp_pts
                else:
                    sl_p = entry_p + sl_pts
                    tp_p = entry_p - tp_pts
        else:
            h = highs[i]
            l = lows[i]
            hit_sl = (l <= sl_p) if dir_val == 1 else (h >= sl_p)
            hit_tp = (h >= tp_p) if dir_val == 1 else (l <= tp_p)
            
            if hit_sl and hit_tp:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    entry_prices[trade_count] = entry_p
                    exit_prices[trade_count] = sl_p
                    pnls[trade_count] = -sl_pts
                    reasons[trade_count] = 3
                    trade_count += 1
                in_pos = False
            elif hit_sl:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    entry_prices[trade_count] = entry_p
                    exit_prices[trade_count] = sl_p
                    pnls[trade_count] = -sl_pts
                    reasons[trade_count] = 1
                    trade_count += 1
                in_pos = False
            elif hit_tp:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    entry_prices[trade_count] = entry_p
                    exit_prices[trade_count] = tp_p
                    pnls[trade_count] = tp_pts
                    reasons[trade_count] = 2
                    trade_count += 1
                in_pos = False
            elif (i - entry_idx) >= max_hold_bars:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    entry_prices[trade_count] = entry_p
                    exit_prices[trade_count] = closes[i]
                    pnls[trade_count] = (closes[i] - entry_p) * dir_val
                    reasons[trade_count] = 4
                    trade_count += 1
                in_pos = False
                
    return (entry_indices[:trade_count], exit_indices[:trade_count], 
            directions[:trade_count], entry_prices[:trade_count], 
            exit_prices[:trade_count], pnls[:trade_count], reasons[:trade_count])

@numba.njit(fastmath=True)
def calc_fvg_signals(highs, lows, min_gap_pts):
    n = len(highs)
    entries = np.zeros(n, dtype=np.int8)
    for i in range(3, n):
        # Strictly lookahead-free: evaluates bar i-1 vs i-3
        if (lows[i - 1] - highs[i - 3]) >= min_gap_pts:
            entries[i] = 1 # Bull FVG momentum entry
        elif (lows[i - 3] - highs[i - 1]) >= min_gap_pts:
            entries[i] = -1 # Bear FVG momentum entry
    return entries

@numba.njit(fastmath=True)
def calc_bollinger_signals(closes, period, std_mult):
    n = len(closes)
    entries = np.zeros(n, dtype=np.int8)
    win_sum = 0.0
    win_sum_sq = 0.0
    for i in range(period):
        win_sum += closes[i]
        win_sum_sq += closes[i] * closes[i]
        
    for i in range(period + 1, n):
        val_add = closes[i - 1]
        val_sub = closes[i - 1 - period]
        win_sum += val_add - val_sub
        win_sum_sq += val_add * val_add - val_sub * val_sub
        
        mean = win_sum / period
        var = (win_sum_sq / period) - (mean * mean)
        if var > 1e-8:
            std = np.sqrt(var)
            upper = mean + std_mult * std
            lower = mean - std_mult * std
            c_prev = closes[i - 1]
            if c_prev <= lower:
                entries[i] = 1 # Bounce long
            elif c_prev >= upper:
                entries[i] = -1 # Fade short
    return entries

@numba.njit(fastmath=True)
def calc_orb_signals(highs, lows, closes, or_mins, stretch_pts):
    n = len(closes)
    entries = np.zeros(n, dtype=np.int8)
    for i in range(or_mins + 1, n):
        cur_h = -1e9
        cur_l = 1e9
        for j in range(i - 1 - or_mins, i - 1):
            if highs[j] > cur_h:
                cur_h = highs[j]
            if lows[j] < cur_l:
                cur_l = lows[j]
        c_prev = closes[i - 1]
        if c_prev > cur_h + stretch_pts:
            entries[i] = 1 # Breakout long
        elif c_prev < cur_l - stretch_pts:
            entries[i] = -1 # Breakout short
    return entries

@numba.njit(fastmath=True)
def calc_vwap_fade_signals(closes, volumes, day_ids, vwap_std):
    n = len(closes)
    entries = np.zeros(n, dtype=np.int8)
    cum_pv = 0.0
    cum_v = 0.0
    cum_pv_sq = 0.0
    cur_day = day_ids[0]
    
    for i in range(1, n):
        if day_ids[i] != cur_day:
            cur_day = day_ids[i]
            cum_pv = 0.0
            cum_v = 0.0
            cum_pv_sq = 0.0
            
        c_prev = closes[i - 1]
        v_prev = volumes[i - 1]
        cum_pv += c_prev * v_prev
        cum_v += v_prev
        cum_pv_sq += (c_prev * c_prev) * v_prev
        
        if cum_v > 0:
            vwap = cum_pv / cum_v
            mean_sq = cum_pv_sq / cum_v
            var = mean_sq - (vwap * vwap)
            if var > 1e-6:
                std = np.sqrt(var)
                upper = vwap + vwap_std * std
                lower = vwap - vwap_std * std
                if c_prev <= lower:
                    entries[i] = 1 # fade long
                elif c_prev >= upper:
                    entries[i] = -1 # fade short
    return entries

# ---------------------------------------------------------------------------
# GLOBAL DATASET STORAGE (FOR ZERO-COPY FORK WORKERS)
# ---------------------------------------------------------------------------
GLOBAL_DATASETS = {}

def evaluate_subsets(e_indices, pnl_pts, symbol, sess_arr):
    pt_val = PT_VALUES[symbol]
    pnl_usd = pnl_pts * pt_val
    n_trades = len(e_indices)
    if n_trades == 0:
        return {}

    trade_sess = sess_arr[e_indices]
    
    subsets = {
        "ALL": np.ones(n_trades, dtype=bool),
        "ASIAN": (trade_sess == 1),
        "LONDON": (trade_sess == 2),
        "NY": (trade_sess == 3),
        "ASIAN_LONDON": (trade_sess == 1) | (trade_sess == 2),
        "LONDON_NY": (trade_sess == 2) | (trade_sess == 3),
        "ASIAN_NY": (trade_sess == 1) | (trade_sess == 3)
    }

    results = {}
    for s_name, mask in subsets.items():
        cnt = int(np.sum(mask))
        if cnt < 5:
            results[s_name] = {
                "trades": cnt,
                "net_pnl": float(np.sum(pnl_usd[mask])) if cnt > 0 else 0.0,
                "win_rate": 0.0,
                "profit_factor": 0.0,
                "sharpe_ratio": 0.0,
                "max_dd_pct": 0.0
            }
            continue

        p_sub = pnl_usd[mask]
        net_pnl = float(np.sum(p_sub))
        wins = p_sub[p_sub > 0]
        losses = p_sub[p_sub < 0]
        win_rate = float(len(wins) / cnt * 100.0)
        profit_factor = float(np.sum(wins) / abs(np.sum(losses))) if abs(np.sum(losses)) > 0 else 99.0
        
        equity = pd.Series(50000.0 + np.cumsum(p_sub))
        t_df = pd.DataFrame({"pnl": p_sub})
        
        # Calculate quant metrics without slow MC/bootstrap
        m = calculate_metrics({"trades": t_df, "equity_curve": equity}, n_mc=0, n_boot=0)
        
        basic = m.get("basic", {})
        sr = float(basic.get("sharpe_ratio", 0.0))
        max_dd = float(basic.get("max_drawdown", 0.0) * 100.0)
        
        # Compute Sortino & Calmar
        rets = equity.pct_change().dropna()
        downside = rets[rets < 0]
        downside_std = float(downside.std()) if len(downside) > 1 else 1e-4
        sortino = float(rets.mean() / downside_std * np.sqrt(252)) if downside_std > 0 else 0.0
        cagr = float(basic.get("cagr", 0.0))
        calmar = float(cagr / (abs(max_dd) / 100.0)) if abs(max_dd) > 0.01 else 0.0
        
        sod_dd = float(m.get("start_of_day_to_trough_drawdown", {}).get("max", 0.0))
        psr = float(m.get("probabilistic_sharpe_ratio", 0.0))
        dsr = float(m.get("deflated_sharpe_ratio", 0.0))
        markov_str = float(m.get("markov_transition_strength", {}).get("strength", 0.0))
        brownian_vr = float(m.get("brownian_motion_test", {}).get("variance_ratio", 1.0))
        b_sharpe = m.get("bayesian_sharpe", {})
        b_mean = float(b_sharpe.get("mean", 0.0))
        b_ci = b_sharpe.get("ci_95", [0.0, 0.0])
        
        regs = m.get("regressions", {})
        r2_lin = float(regs.get("linear", {}).get("r2", 0.0))
        r2_quad = float(regs.get("quadratic", {}).get("r2", 0.0))
        r2_poly = float(regs.get("polynomial", {}).get("r2", 0.0))

        results[s_name] = {
            "trades": cnt,
            "net_pnl": net_pnl,
            "win_rate": win_rate,
            "profit_factor": profit_factor,
            "sharpe_ratio": sr,
            "sortino": sortino,
            "calmar": calmar,
            "max_dd_pct": max_dd,
            "sod_drawdown_max": sod_dd,
            "psr": psr,
            "dsr": dsr,
            "markov_strength": markov_str,
            "brownian_vr": brownian_vr,
            "bayesian_sharpe_mean": b_mean,
            "bayesian_ci_95": b_ci,
            "r2_linear": r2_lin,
            "r2_quadratic": r2_quad,
            "r2_cubic": r2_poly
        }

    return results

def worker_eval(task):
    strat, sym, params = task
    d = GLOBAL_DATASETS[sym]
    opens, highs, lows, closes, volumes, day_ids, sess_arr = d
    tick = TICK_SIZES[sym]
    
    tp = float(params["tp_pts"])
    sl = float(params["sl_pts"])
    hold = int(params.get("max_hold_bars", 120))
    
    if strat == "FVG":
        min_gap_pts = float(params["min_gap_ticks"]) * tick
        entries = calc_fvg_signals(highs, lows, min_gap_pts)
    elif strat == "BOLLINGER_BANDS":
        entries = calc_bollinger_signals(closes, int(params["period"]), float(params["std_mult"]))
    elif strat == "FLEXING_JOE_ORB":
        stretch_pts = float(params["stretch_mult"]) * tick * 10.0
        entries = calc_orb_signals(highs, lows, closes, int(params["or_mins"]), stretch_pts)
    elif strat == "VWAP_FADE":
        entries = calc_vwap_fade_signals(closes, volumes, day_ids, float(params["vwap_std"]))
    else:
        return None
        
    e_idx, x_idx, dirs, ep, xp, pnl_pts, reasons = simulate_fast(opens, highs, lows, closes, entries, sl, tp, hold)
    session_res = evaluate_subsets(e_idx, pnl_pts, sym, sess_arr)
    
    return {
        "timestamp": datetime.now().isoformat(),
        "strategy": strat,
        "symbol": sym,
        "params": params,
        "sessions": session_res
    }

def update_leaderboard(records):
    """Dynamically write top 25 leaderboard markdown."""
    valid = [r for r in records if "ALL" in r.get("sessions", {}) and r["sessions"]["ALL"].get("trades", 0) >= 30]
    valid.sort(key=lambda x: -x["sessions"]["ALL"].get("sharpe_ratio", 0.0))

    lines = [
        "# LIVE 10-YEAR MULTI-ASSET RECURSIVE QUANT SWEEP LEADERBOARD",
        f"> **Last Updated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S ET')}  ",
        f"> **Total Evaluated Parameter Sets:** {len(records):,}  ",
        f"> **Hardware Node:** Akpan Laptop WSL2 (16 CPU Threads)  ",
        f"> **Datasets Evaluated:** ES, NQ, YM (10.35M total 1-min bars from 2016 to 2026)  ",
        "",
        "| Rank | Strategy | Asset | Net PnL ($) | Win Rate (%) | Trades | Sharpe | Sortino | Max DD (%) | DSR | Best Session | Best Session Sharpe | Key Parameters |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
    ]

    for rank, r in enumerate(valid[:25], 1):
        all_s = r["sessions"]["ALL"]
        best_sess_name = "ALL"
        best_sess_sr = all_s.get("sharpe_ratio", 0.0)
        for s_name, s_data in r["sessions"].items():
            if s_data.get("trades", 0) >= 15 and s_data.get("sharpe_ratio", 0.0) > best_sess_sr:
                best_sess_sr = s_data.get("sharpe_ratio", 0.0)
                best_sess_name = s_name

        p_str = ", ".join(f"{k}:{v}" for k, v in r["params"].items())
        lines.append(
            f"| **{rank}** | `{r['strategy']}` | **{r['symbol']}** | "
            f"**${all_s.get('net_pnl',0.0):,.2f}** | {all_s.get('win_rate',0.0):.1f}% | {all_s.get('trades',0)} | "
            f"**{all_s.get('sharpe_ratio',0.0):.2f}** | {all_s.get('sortino',0.0):.2f} | {all_s.get('max_dd_pct',0.0):.2f}% | "
            f"{all_s.get('dsr',0.0):.2f} | `{best_sess_name}` | **{best_sess_sr:.2f}** | `{p_str}` |"
        )

    with open(LEADERBOARD_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

def main():
    print("="*75)
    print("STARTING 10-YEAR MULTI-ASSET RECURSIVE QUANT SWEEP ON AKPAN LAPTOP")
    print("Zero Cloud VM Compute | 16 Laptop Cores | 10.35M Bars | 4 Strategies")
    print("="*75)

    symbols = ["NQ", "ES", "YM"]
    for sym in symbols:
        p = _ROOT / "data" / f"{sym}_1min.parquet"
        print(f"Loading {sym} from {p.name}...")
        df = pd.read_parquet(p)
        
        # Pre-classify sessions in ET timezone
        et_times = df.index.tz_convert("America/New_York")
        et_min = et_times.hour * 60 + et_times.minute
        
        sess_arr = np.zeros(len(df), dtype=np.int8)
        # Asian: 18:00 (1080) to 02:00 (120) -> 1
        sess_arr[(et_min >= 1080) | (et_min < 120)] = 1
        # London: 02:00 (120) to 08:30 (510) -> 2
        sess_arr[(et_min >= 120) & (et_min < 510)] = 2
        # NY: 08:30 (510) to 16:10 (970) -> 3
        sess_arr[(et_min >= 510) & (et_min <= 970)] = 3
        
        # Day IDs for daily VWAP reset
        day_ids = (et_times.year * 10000 + et_times.month * 100 + et_times.day).values
        
        GLOBAL_DATASETS[sym] = (
            df['open'].values,
            df['high'].values,
            df['low'].values,
            df['close'].values,
            df['volume'].values if 'volume' in df.columns else np.ones(len(df), dtype=np.float64),
            day_ids,
            sess_arr
        )
        print(f" -> {sym}: {len(df):,} bars pre-processed into memory.")

    # JIT warmup on dummy arrays
    d = GLOBAL_DATASETS["NQ"]
    calc_fvg_signals(d[1][:100], d[2][:100], 4.0)
    calc_bollinger_signals(d[3][:100], 20, 2.0)
    calc_orb_signals(d[1][:100], d[2][:100], d[3][:100], 15, 10.0)
    calc_vwap_fade_signals(d[3][:100], d[4][:100], d[5][:100], 2.0)
    simulate_fast(d[0][:100], d[1][:100], d[2][:100], d[3][:100], np.zeros(100, dtype=np.int8), 10.0, 20.0, 60)
    print("Numba JIT compilation warm-up complete.")

    # -------------------------------------------------------------
    # STAGE 1: COARSE GRID GENERATION
    # -------------------------------------------------------------
    print("\n[STAGE 1] Generating Coarse Recursive Grid across 4 strategies...")
    coarse_tasks = []

    # 1. FVG
    for sym in symbols:
        for gap in [2, 4, 8, 12, 16, 24]:
            for tp in [15.0, 25.0, 40.0, 60.0, 80.0]:
                for sl in [10.0, 15.0, 25.0, 40.0]:
                    for hold in [30, 60, 120]:
                        coarse_tasks.append(("FVG", sym, {"min_gap_ticks": gap, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # 2. Bollinger Bands
    for sym in symbols:
        for period in [10, 20, 30, 50]:
            for std in [1.5, 2.0, 2.5, 3.0]:
                for tp in [10.0, 20.0, 30.0, 50.0]:
                    for sl in [10.0, 20.0, 30.0, 50.0]:
                        for hold in [30, 60, 120]:
                            coarse_tasks.append(("BOLLINGER_BANDS", sym, {"period": period, "std_mult": std, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # 3. Flexing Joe ORB
    for sym in symbols:
        for or_m in [5, 15, 30]:
            for stretch in [0.5, 1.0, 1.5, 2.0]:
                for tp in [15.0, 30.0, 50.0, 80.0]:
                    for sl in [15.0, 30.0, 45.0]:
                        for hold in [30, 60, 120]:
                            coarse_tasks.append(("FLEXING_JOE_ORB", sym, {"or_mins": or_m, "stretch_mult": stretch, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # 4. Scalp VWAP Fade
    for sym in symbols:
        for std in [1.5, 2.0, 2.5, 3.0]:
            for tp in [10.0, 20.0, 30.0, 45.0]:
                for sl in [10.0, 20.0, 30.0, 45.0]:
                    for hold in [30, 60, 120]:
                        coarse_tasks.append(("VWAP_FADE", sym, {"vwap_std": std, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    print(f"Total Stage 1 Coarse Tasks: {len(coarse_tasks):,} combinations across ES, NQ, YM.")
    print("Launching multiprocessing pool on 16 laptop threads...")

    completed_records = []
    t_start = time.perf_counter()

    with mp.Pool(16) as pool:
        for i, record in enumerate(pool.imap_unordered(worker_eval, coarse_tasks, chunksize=16), 1):
            if record is not None:
                completed_records.append(record)
                with open(STREAM_FILE, "a") as sf:
                    sf.write(json.dumps(record) + "\n")

            if i % 100 == 0 or i == len(coarse_tasks):
                elapsed = time.perf_counter() - t_start
                rate = i / elapsed
                rem = (len(coarse_tasks) - i) / rate if rate > 0 else 0.0
                print(f"[Stage 1: {i}/{len(coarse_tasks)}] ({i/len(coarse_tasks)*100:.1f}%) | {rate:.1f} evals/sec | ETA: {rem:.1f}s")
                update_leaderboard(completed_records)

    print(f"Stage 1 Coarse Sweep Finished in {time.perf_counter() - t_start:.2f} seconds!")

    # -------------------------------------------------------------
    # STAGE 2: RECURSIVE FINE ZOOM
    # -------------------------------------------------------------
    print("\n[STAGE 2] Identifying Prime Neighborhoods for Granular Recursive Refinement...")
    fine_tasks = []
    
    # Select top 3 parameter sets per (strategy, symbol)
    for strat in ["FVG", "BOLLINGER_BANDS", "FLEXING_JOE_ORB", "VWAP_FADE"]:
        for sym in symbols:
            cand = [r for r in completed_records if r["strategy"] == strat and r["symbol"] == sym and "ALL" in r.get("sessions", {})]
            cand.sort(key=lambda x: -x["sessions"]["ALL"].get("sharpe_ratio", 0.0))
            top_3 = cand[:3]
            
            for c in top_3:
                p = c["params"]
                if strat == "FVG":
                    gap = p["min_gap_ticks"]
                    tp = p["tp_pts"]
                    sl = p["sl_pts"]
                    hold = p["max_hold_bars"]
                    for d_gap in [-1, 0, 1]:
                        for d_tp in [-5.0, 0.0, 5.0]:
                            for d_sl in [-5.0, 0.0, 5.0]:
                                n_gap = max(1, gap + d_gap)
                                n_tp = max(5.0, tp + d_tp)
                                n_sl = max(5.0, sl + d_sl)
                                fine_tasks.append(("FVG", sym, {"min_gap_ticks": n_gap, "tp_pts": n_tp, "sl_pts": n_sl, "max_hold_bars": hold}))
                elif strat == "BOLLINGER_BANDS":
                    period = p["period"]
                    std = p["std_mult"]
                    tp = p["tp_pts"]
                    sl = p["sl_pts"]
                    hold = p["max_hold_bars"]
                    for d_per in [-2, 0, 2]:
                        for d_std in [-0.2, 0.0, 0.2]:
                            for d_tp in [-5.0, 0.0, 5.0]:
                                for d_sl in [-5.0, 0.0, 5.0]:
                                    fine_tasks.append(("BOLLINGER_BANDS", sym, {"period": max(5, period + d_per), "std_mult": round(max(1.0, std + d_std), 2), "tp_pts": max(5.0, tp + d_tp), "sl_pts": max(5.0, sl + d_sl), "max_hold_bars": hold}))
                elif strat == "FLEXING_JOE_ORB":
                    or_m = p["or_mins"]
                    stretch = p["stretch_mult"]
                    tp = p["tp_pts"]
                    sl = p["sl_pts"]
                    hold = p["max_hold_bars"]
                    for d_str in [-0.2, 0.0, 0.2]:
                        for d_tp in [-5.0, 0.0, 5.0]:
                            for d_sl in [-5.0, 0.0, 5.0]:
                                fine_tasks.append(("FLEXING_JOE_ORB", sym, {"or_mins": or_m, "stretch_mult": round(max(0.2, stretch + d_str), 2), "tp_pts": max(5.0, tp + d_tp), "sl_pts": max(5.0, sl + d_sl), "max_hold_bars": hold}))
                elif strat == "VWAP_FADE":
                    std = p["vwap_std"]
                    tp = p["tp_pts"]
                    sl = p["sl_pts"]
                    hold = p["max_hold_bars"]
                    for d_std in [-0.2, 0.0, 0.2]:
                        for d_tp in [-5.0, 0.0, 5.0]:
                            for d_sl in [-5.0, 0.0, 5.0]:
                                fine_tasks.append(("VWAP_FADE", sym, {"vwap_std": round(max(1.0, std + d_std), 2), "tp_pts": max(5.0, tp + d_tp), "sl_pts": max(5.0, sl + d_sl), "max_hold_bars": hold}))

    # Deduplicate fine tasks
    unique_fine = []
    seen = set()
    for item in fine_tasks:
        key = (item[0], item[1], json.dumps(item[2], sort_keys=True))
        if key not in seen:
            seen.add(key)
            unique_fine.append(item)

    print(f"Total Stage 2 Fine Recursive Tasks: {len(unique_fine):,} combinations.")
    t_fine = time.perf_counter()

    with mp.Pool(16) as pool:
        for i, record in enumerate(pool.imap_unordered(worker_eval, unique_fine, chunksize=16), 1):
            if record is not None:
                completed_records.append(record)
                with open(STREAM_FILE, "a") as sf:
                    sf.write(json.dumps(record) + "\n")

            if i % 100 == 0 or i == len(unique_fine):
                elapsed = time.perf_counter() - t_fine
                rate = i / elapsed
                rem = (len(unique_fine) - i) / rate if rate > 0 else 0.0
                print(f"[Stage 2: {i}/{len(unique_fine)}] ({i/len(unique_fine)*100:.1f}%) | {rate:.1f} evals/sec | ETA: {rem:.1f}s")
                update_leaderboard(completed_records)

    total_time = time.perf_counter() - t_start
    print("\n" + "="*75)
    print(f"RECURSIVE MULTI-RESOLUTION SWEEP COMPLETED SUCCESSFULLY IN {total_time:.2f}s ({total_time/60:.2f} min)!")
    print(f"Total Parameter Combinations Evaluated: {len(completed_records):,}")
    print(f"Stream Results Log: {STREAM_FILE}")
    print(f"Final Live Leaderboard: {LEADERBOARD_FILE}")
    print("="*75)

if __name__ == "__main__":
    main()
