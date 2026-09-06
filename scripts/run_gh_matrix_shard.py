import sys, time, json, os, argparse
from pathlib import Path
import numpy as np
import pandas as pd
from datetime import datetime
import numba

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT))

from topstep_strats.metrics import calculate_metrics

PT_VALUES = {"ES": 50.0, "NQ": 20.0, "YM": 5.0}
TICK_SIZES = {"ES": 0.25, "NQ": 0.25, "YM": 1.0}

@numba.njit(fastmath=True)
def simulate_fast(opens, highs, lows, closes, entries, sl_pts, tp_pts, max_hold_bars=120):
    n = len(closes)
    max_trades = 150000
    entry_indices = np.empty(max_trades, dtype=np.int32)
    exit_indices = np.empty(max_trades, dtype=np.int32)
    directions = np.empty(max_trades, dtype=np.int8)
    pnls = np.empty(max_trades, dtype=np.float64)
    reasons = np.empty(max_trades, dtype=np.int8)
    
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
                    pnls[trade_count] = -sl_pts
                    reasons[trade_count] = 3
                    trade_count += 1
                in_pos = False
            elif hit_sl:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    pnls[trade_count] = -sl_pts
                    reasons[trade_count] = 1
                    trade_count += 1
                in_pos = False
            elif hit_tp:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    pnls[trade_count] = tp_pts
                    reasons[trade_count] = 2
                    trade_count += 1
                in_pos = False
            elif (i - entry_idx) >= max_hold_bars:
                if trade_count < max_trades:
                    entry_indices[trade_count] = entry_idx
                    exit_indices[trade_count] = i
                    directions[trade_count] = dir_val
                    pnls[trade_count] = (closes[i] - entry_p) * dir_val
                    reasons[trade_count] = 4
                    trade_count += 1
                in_pos = False
                
    return (entry_indices[:trade_count], exit_indices[:trade_count], 
            directions[:trade_count], pnls[:trade_count], reasons[:trade_count])

@numba.njit(fastmath=True)
def calc_fvg_signals(highs, lows, min_gap_pts):
    n = len(highs)
    entries = np.zeros(n, dtype=np.int8)
    for i in range(3, n):
        if (lows[i - 1] - highs[i - 3]) >= min_gap_pts:
            entries[i] = 1
        elif (lows[i - 3] - highs[i - 1]) >= min_gap_pts:
            entries[i] = -1
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
                entries[i] = 1
            elif c_prev >= upper:
                entries[i] = -1
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
            entries[i] = 1
        elif c_prev < cur_l - stretch_pts:
            entries[i] = -1
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
                    entries[i] = 1
                elif c_prev >= upper:
                    entries[i] = -1
    return entries

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
        m = calculate_metrics({"trades": t_df, "equity_curve": equity}, n_mc=0, n_boot=0)
        
        basic = m.get("basic", {})
        sr = float(basic.get("sharpe_ratio", 0.0))
        max_dd = float(basic.get("max_drawdown", 0.0) * 100.0)
        
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

def main():
    parser = argparse.ArgumentParser(description="Run 10-Year Matrix Quant Sweep Shard on GitHub Actions")
    parser.add_argument("--shard", type=int, required=True, help="Shard index (0-based)")
    parser.add_argument("--total-shards", type=int, default=20, help="Total shards")
    parser.add_argument("--data-dir", type=str, default="data", help="Directory containing parquet files")
    parser.add_argument("--output", type=str, default=None, help="Output JSONL path")
    args = parser.parse_args()

    output_path = Path(args.output if args.output else f"results_shard_{args.shard}.jsonl")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"=== Starting GitHub Actions Shard {args.shard + 1}/{args.total_shards} ===")
    data_dir = Path(args.data_dir)
    symbols = ["NQ", "ES", "YM"]
    datasets = {}

    for sym in symbols:
        p = data_dir / f"{sym}_1min.parquet"
        print(f"Loading {sym} from {p}...")
        df = pd.read_parquet(p)
        et_times = df.index.tz_convert("America/New_York")
        et_min = et_times.hour * 60 + et_times.minute
        
        sess_arr = np.zeros(len(df), dtype=np.int8)
        sess_arr[(et_min >= 1080) | (et_min < 120)] = 1
        sess_arr[(et_min >= 120) & (et_min < 510)] = 2
        sess_arr[(et_min >= 510) & (et_min <= 970)] = 3
        
        day_ids = (et_times.year * 10000 + et_times.month * 100 + et_times.day).values
        datasets[sym] = (
            df['open'].values,
            df['high'].values,
            df['low'].values,
            df['close'].values,
            df['volume'].values if 'volume' in df.columns else np.ones(len(df), dtype=np.float64),
            day_ids,
            sess_arr
        )
        print(f" -> {sym}: {len(df):,} bars ready.")

    # Generate Full Parameter Task Matrix
    all_tasks = []
    # 1. FVG
    for sym in symbols:
        for gap in [2, 4, 8, 12, 16, 24]:
            for tp in [15.0, 25.0, 40.0, 60.0, 80.0]:
                for sl in [10.0, 15.0, 25.0, 40.0]:
                    for hold in [30, 60, 120]:
                        all_tasks.append(("FVG", sym, {"min_gap_ticks": gap, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # 2. Bollinger Bands
    for sym in symbols:
        for period in [10, 20, 30, 50]:
            for std in [1.5, 2.0, 2.5, 3.0]:
                for tp in [10.0, 20.0, 30.0, 50.0]:
                    for sl in [10.0, 20.0, 30.0, 50.0]:
                        for hold in [30, 60, 120]:
                            all_tasks.append(("BOLLINGER_BANDS", sym, {"period": period, "std_mult": std, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # 3. Flexing Joe ORB
    for sym in symbols:
        for or_m in [5, 15, 30]:
            for stretch in [0.5, 1.0, 1.5, 2.0]:
                for tp in [15.0, 30.0, 50.0, 80.0]:
                    for sl in [15.0, 30.0, 45.0]:
                        for hold in [30, 60, 120]:
                            all_tasks.append(("FLEXING_JOE_ORB", sym, {"or_mins": or_m, "stretch_mult": stretch, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # 4. Scalp VWAP Fade
    for sym in symbols:
        for std in [1.5, 2.0, 2.5, 3.0]:
            for tp in [10.0, 20.0, 30.0, 45.0]:
                for sl in [10.0, 20.0, 30.0, 45.0]:
                    for hold in [30, 60, 120]:
                        all_tasks.append(("VWAP_FADE", sym, {"vwap_std": std, "tp_pts": tp, "sl_pts": sl, "max_hold_bars": hold}))

    # Partition tasks for this shard
    shard_tasks = [t for idx, t in enumerate(all_tasks) if idx % args.total_shards == args.shard]
    print(f"Total Universe Tasks: {len(all_tasks):,} | Shard Assigned Tasks: {len(shard_tasks):,}")

    t0 = time.perf_counter()
    with open(output_path, "w") as out_f:
        for i, (strat, sym, params) in enumerate(shard_tasks, 1):
            d = datasets[sym]
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
                continue

            e_idx, x_idx, dirs, pnl_pts, reasons = simulate_fast(opens, highs, lows, closes, entries, sl, tp, hold)
            session_res = evaluate_subsets(e_idx, pnl_pts, sym, sess_arr)

            record = {
                "timestamp": datetime.now().isoformat(),
                "shard": args.shard,
                "strategy": strat,
                "symbol": sym,
                "params": params,
                "sessions": session_res
            }
            out_f.write(json.dumps(record) + "\n")
            if i % 25 == 0 or i == len(shard_tasks):
                elapsed = time.perf_counter() - t0
                print(f"[{i}/{len(shard_tasks)}] Processed in {elapsed:.1f}s ({i/elapsed:.1f} evals/sec)")

    print(f"Shard {args.shard} finished in {time.perf_counter() - t0:.2f}s. Results written to {output_path}")

if __name__ == "__main__":
    main()
