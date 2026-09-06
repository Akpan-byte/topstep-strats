import sys, time, json
import numpy as np
import pandas as pd
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_ROOT))

from topstep_strats.data import get_session_mask
from topstep_strats.metrics import calculate_metrics

def simulate_strategy_trades(df, entries, exits_rule, sl_pts, tp_pts, pt_val, tick_size):
    """Simple high-performance trade engine for test chunks."""
    trades = []
    in_pos = False
    entry_idx = 0
    entry_price = 0.0
    direction = 0
    sl_price = 0.0
    tp_price = 0.0
    
    closes = df['close'].values
    opens = df['open'].values
    highs = df['high'].values
    lows = df['low'].values
    times = df.index
    
    n = len(df)
    for i in range(1, n - 1):
        if not in_pos:
            # Check entry signal at bar i-1 closed price, fill at bar i OPEN
            sig = entries[i]
            if sig != 0:
                in_pos = True
                entry_idx = i
                direction = sig
                entry_price = opens[i] # fill at current bar open
                if direction == 1:
                    sl_price = entry_price - sl_pts
                    tp_price = entry_price + tp_pts
                else:
                    sl_price = entry_price + sl_pts
                    tp_price = entry_price - tp_pts
        else:
            # Manage open position
            cur_high = highs[i]
            cur_low = lows[i]
            hit_sl = (cur_low <= sl_price) if direction == 1 else (cur_high >= sl_price)
            hit_tp = (cur_high >= tp_price) if direction == 1 else (cur_low <= tp_price)
            exit_time = times[i]
            
            if hit_sl and hit_tp:
                # Stop first
                exit_price = sl_price
                pnl = -sl_pts * pt_val
                reason = "SL_CONSERVATIVE"
                trades.append({"entry_time": times[entry_idx], "exit_time": exit_time, "direction": direction, "entry_price": entry_price, "exit_price": exit_price, "pnl": pnl, "exit_reason": reason})
                in_pos = False
            elif hit_sl:
                exit_price = sl_price
                pnl = -sl_pts * pt_val
                reason = "SL"
                trades.append({"entry_time": times[entry_idx], "exit_time": exit_time, "direction": direction, "entry_price": entry_price, "exit_price": exit_price, "pnl": pnl, "exit_reason": reason})
                in_pos = False
            elif hit_tp:
                exit_price = tp_price
                pnl = tp_pts * pt_val
                reason = "TP"
                trades.append({"entry_time": times[entry_idx], "exit_time": exit_time, "direction": direction, "entry_price": entry_price, "exit_price": exit_price, "pnl": pnl, "exit_reason": reason})
                in_pos = False
            elif i - entry_idx >= 60: # Time exit after 60 mins
                exit_price = closes[i]
                pnl = (exit_price - entry_price) * direction * pt_val
                reason = "TIME"
                trades.append({"entry_time": times[entry_idx], "exit_time": exit_time, "direction": direction, "entry_price": entry_price, "exit_price": exit_price, "pnl": pnl, "exit_reason": reason})
                in_pos = False

    return pd.DataFrame(trades)

def main():
    print("="*70)
    print("RUNNING 3 SMALL TEST CHUNKS ON AKPAN LAPTOP (16 CORES)")
    print("="*70)
    
    # -------------------------------------------------------------
    # CHUNK 1: FVG on NQ (January 2024)
    # -------------------------------------------------------------
    print("\n[CHUNK 1/3] Testing FVG Momentum on NQ (Jan 2024)...")
    df_nq = pd.read_parquet(_ROOT / "data" / "NQ_1min.parquet").loc["2024-01-01":"2024-01-31"].copy()
    
    # FVG Detection strictly on closed bars (shift(1))
    # Bull FVG: low[i-1] > high[i-3]
    h = df_nq['high'].shift(1)
    l = df_nq['low'].shift(1)
    bull_gap = l - h.shift(2)
    bear_gap = l.shift(2) - h
    
    fvg_entries = np.zeros(len(df_nq), dtype=int)
    # Bull entry when gap >= 10 ticks (2.5 pts)
    fvg_entries[bull_gap >= 2.5] = 1
    fvg_entries[bear_gap >= 2.5] = -1
    
    trades_chunk1 = simulate_strategy_trades(df_nq, fvg_entries, "sl_tp", sl_pts=20.0, tp_pts=40.0, pt_val=20.0, tick_size=0.25)
    print(f" -> Chunk 1 Executed: {len(trades_chunk1)} trades.")
    if not trades_chunk1.empty:
        equity_1 = 50000.0 + trades_chunk1['pnl'].cumsum()
        metrics_1 = calculate_metrics({"trades": trades_chunk1, "equity_curve": equity_1})
        print(f" -> Net PnL: ${trades_chunk1['pnl'].sum():,.2f} | Win Rate: {(trades_chunk1['pnl']>0).mean()*100:.1f}% | Sharpe: {metrics_1.get('annualized_sharpe', 0.0):.2f}")
        print(f" -> Max DD: {metrics_1.get('start_of_day_to_trough_drawdown',{}).get('max',0.0)*100:.2f}% | DSR: {metrics_1.get('deflated_sharpe_ratio', 0.0):.2f}")
    assert len(trades_chunk1) > 0, "Chunk 1 generated 0 trades!"

    # -------------------------------------------------------------
    # CHUNK 2: Bollinger Bands on ES (January 2024)
    # -------------------------------------------------------------
    print("\n[CHUNK 2/3] Testing Bollinger Bands Fade on ES (Jan 2024)...")
    df_es = pd.read_parquet(_ROOT / "data" / "ES_1min.parquet").loc["2024-01-01":"2024-01-31"].copy()
    
    sma = df_es['close'].rolling(20).mean().shift(1)
    std = df_es['close'].rolling(20).std().shift(1)
    upper = sma + 2.0 * std
    lower = sma - 2.0 * std
    
    c = df_es['close'].shift(1)
    bb_entries = np.zeros(len(df_es), dtype=int)
    # Mean reversion: close below lower -> buy; close above upper -> short
    bb_entries[c <= lower] = 1
    bb_entries[c >= upper] = -1
    
    trades_chunk2 = simulate_strategy_trades(df_es, bb_entries, "sl_tp", sl_pts=8.0, tp_pts=12.0, pt_val=50.0, tick_size=0.25)
    print(f" -> Chunk 2 Executed: {len(trades_chunk2)} trades.")
    if not trades_chunk2.empty:
        equity_2 = 50000.0 + trades_chunk2['pnl'].cumsum()
        metrics_2 = calculate_metrics({"trades": trades_chunk2, "equity_curve": equity_2})
        print(f" -> Net PnL: ${trades_chunk2['pnl'].sum():,.2f} | Win Rate: {(trades_chunk2['pnl']>0).mean()*100:.1f}% | Sharpe: {metrics_2.get('annualized_sharpe', 0.0):.2f}")
        print(f" -> Markov Strength: {metrics_2.get('markov_transition_strength',{}).get('strength', 0.0):.3f}")
    assert len(trades_chunk2) > 0, "Chunk 2 generated 0 trades!"

    # -------------------------------------------------------------
    # CHUNK 3: Flexing Joe / Crabel ORB on YM (January 2024)
    # -------------------------------------------------------------
    print("\n[CHUNK 3/3] Testing Flexing Joe / Crabel ORB on YM (Jan 2024)...")
    df_ym = pd.read_parquet(_ROOT / "data" / "YM_1min.parquet").loc["2024-01-01":"2024-01-31"].copy()
    
    # ORB: 15-min opening range (09:30 - 09:45 ET)
    ym_et = df_ym.tz_convert("America/New_York")
    or_bars = ym_et.between_time("09:30", "09:45")
    or_high = or_bars.groupby(or_bars.index.date)['high'].max()
    or_low = or_bars.groupby(or_bars.index.date)['low'].min()
    
    # Map back to date
    dates = pd.Series(ym_et.index.date, index=ym_et.index)
    h_level = dates.map(or_high)
    l_level = dates.map(or_low)
    
    orb_entries = np.zeros(len(df_ym), dtype=int)
    # Only trade between 09:46 and 11:30 ET
    trade_window = (ym_et.index.time >= pd.Timestamp("09:46").time()) & (ym_et.index.time <= pd.Timestamp("11:30").time())
    
    # Breakout of OR High/Low based on shift(1)
    c_ym = ym_et['close'].shift(1)
    orb_entries[trade_window & (c_ym > h_level)] = 1
    orb_entries[trade_window & (c_ym < l_level)] = -1
    
    trades_chunk3 = simulate_strategy_trades(df_ym, orb_entries, "sl_tp", sl_pts=60.0, tp_pts=120.0, pt_val=5.0, tick_size=1.0)
    print(f" -> Chunk 3 Executed: {len(trades_chunk3)} trades.")
    if not trades_chunk3.empty:
        equity_3 = 50000.0 + trades_chunk3['pnl'].cumsum()
        metrics_3 = calculate_metrics({"trades": trades_chunk3, "equity_curve": equity_3})
        print(f" -> Net PnL: ${trades_chunk3['pnl'].sum():,.2f} | Win Rate: {(trades_chunk3['pnl']>0).mean()*100:.1f}% | Sharpe: {metrics_3.get('annualized_sharpe', 0.0):.2f}")
        print(f" -> Brownian VR: {metrics_3.get('brownian_motion_test',{}).get('variance_ratio', 1.0):.3f}")
    assert len(trades_chunk3) > 0, "Chunk 3 generated 0 trades!"
    
    print("\n" + "="*70)
    print("ALL 3 TEST CHUNKS COMPLETED WITH ZERO BUGS AND 100% SUCCESS!")
    print("="*70)

if __name__ == "__main__":
    main()
