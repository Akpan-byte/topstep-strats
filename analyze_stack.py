import sys
sys.path.insert(0, "/home/akpan/topstep-strats")
sys.path.insert(0, "/home/akpan/topstep-strats/engine_rust/python")
import pandas as pd
import numpy as np
from topstep_strats.data import load_market_data, get_session_mask, split_by_date
from topstep_strats.strategies.paper1_matrix import generate_signals as gen_paper1, get_strategy_config as cfg_paper1

# Top strategies to stack
legs = [
    {"name": "51_NQ_London_holdday", "sid": 51, "inst": "NQ", "sess": "London", "tp": 0.3, "sl": 10.0, "mode": "hold_day"},
    {"name": "3_NQ_London_holdday", "sid": 3, "inst": "NQ", "sess": "London", "tp": 0.3, "sl": 8.0, "mode": "hold_day"},
    {"name": "9_NQ_NY_trail", "sid": 9, "inst": "NQ", "sess": "NY", "tp": 2.0, "sl": 10.0, "mode": "trail_0.5x"},
    {"name": "13_NQ_NY_breakeven", "sid": 13, "inst": "NQ", "sess": "NY", "tp": 2.0, "sl": 10.0, "mode": "breakeven_0.5x"},
    {"name": "58_NQ_London_trail", "sid": 58, "inst": "NQ", "sess": "London", "tp": 2.0, "sl": 10.0, "mode": "trail_1.0x"},
    {"name": "61_NQ_NY_breakeven", "sid": 61, "inst": "NQ", "sess": "NY", "tp": 2.0, "sl": 6.0, "mode": "breakeven_0.5x"},
]

SESSIONS = {
    "Asian": ("20:00", "23:00"),
    "London": ("03:00", "11:00"),
    "NY": ("09:30", "16:00"),
}

def get_signals(leg):
    path = f"/home/akpan/topstep-strats/data/{leg['inst']}_1min.parquet"
    df = load_market_data(path)
    cfg = cfg_paper1(f"{leg['sid']:03d}")
    cfg.update({
        "instrument": leg["inst"],
        "tick_size": 0.25 if leg["inst"] in ("NQ", "ES") else 1.0,
        "point_value": 20.0 if leg["inst"] == "NQ" else (50.0 if leg["inst"] == "ES" else 5.0),
        "session": leg["sess"],
        "session_start": SESSIONS[leg["sess"]][0],
        "session_end": SESSIONS[leg["sess"]][1],
        "tp_atr": leg["tp"],
        "sl_atr": leg["sl"],
        "session_only": True,
        "one_trade_per_day": True,
        "stop_first": True,
    })
    mask = get_session_mask(df, cfg["session_start"], cfg["session_end"], cfg["tz"])
    df_s = split_by_date(df.loc[mask].copy(), "2016-06-01", "2026-05-29")
    sigs = gen_paper1(df_s, cfg, simulate_exits=False)
    if sigs.empty:
        return pd.DataFrame()
    sigs = sigs.copy()
    sigs["leg"] = leg["name"]
    sigs["priority"] = legs.index(leg)
    return sigs[["entry_time", "direction", "entry_price", "atr_value", "leg", "priority"]]

print("Generating signals...")
all_sigs = []
for leg in legs:
    s = get_signals(leg)
    print(f"  {leg['name']}: {len(s)} signals")
    if not s.empty:
        all_sigs.append(s)

sigs = pd.concat(all_sigs, ignore_index=True)
sigs["entry_time"] = pd.to_datetime(sigs["entry_time"]).dt.tz_convert("America/New_York")
sigs = sigs.sort_values("entry_time").reset_index(drop=True)

# Session end in ET for each entry
def sess_end(ts):
    base = ts.replace(hour=0, minute=0, second=0, microsecond=0)
    if ts.hour >= 18:
        return base + pd.Timedelta(hours=23)
    elif ts.hour >= 3 and ts.hour < 11:
        return base + pd.Timedelta(hours=11)
    else:
        return base + pd.Timedelta(hours=16)

sigs["session_end"] = sigs["entry_time"].apply(sess_end)

# Stack simulator with 2-contract max and priority by list order
active = []
for idx, row in sigs.iterrows():
    # Release expired positions
    active = [a for a in active if a["end"] > row["entry_time"]]
    if len(active) < 2:
        active.append({"end": row["session_end"], "leg": row["leg"]})
        sigs.at[idx, "taken"] = True
        sigs.at[idx, "contracts"] = 1
    else:
        sigs.at[idx, "taken"] = False
        sigs.at[idx, "contracts"] = 0

sigs["taken"] = sigs["taken"].astype(bool)
print(f"\nTotal signals: {len(sigs)}")
print(f"Taken signals: {sigs['taken'].sum()}")
print(f"Skipped signals: {(~sigs['taken']).sum()}")
print("\nTaken by leg:")
print(sigs[sigs["taken"]].groupby("leg").size())

# Estimate PnL scaling from sweep avg_per_trade
sweep = pd.read_csv("gh_results/rust_sweep_v3/paper1_rust_sweep.csv")
leg_pnl = {}
for leg in legs:
    row = sweep[(sweep["strategy_id"] == leg["sid"]) &
                (sweep["instrument"] == leg["inst"]) &
                (sweep["session"] == leg["sess"]) &
                (sweep["tp"] == leg["tp"]) &
                (sweep["sl"] == leg["sl"]) &
                (sweep["mode"] == leg["mode"])]
    leg_pnl[leg["name"]] = row.iloc[0]["avg_per_trade"] if not row.empty else 0.0

sigs["est_pnl"] = sigs["leg"].map(leg_pnl) * sigs["contracts"]
print(f"\nEstimated total PnL (1 contract per taken trade): ${sigs['est_pnl'].sum():,.2f}")
print(f"Estimated avg/week: ${sigs['est_pnl'].sum() / 10 / 52:,.2f}")

# Single best leg at 2 contracts
best_leg = "51_NQ_London_holdday"
best_sigs = sigs[sigs["leg"] == best_leg]
print(f"\n{best_leg} alone at 2 contracts: est avg/week ${best_sigs['est_pnl'].sum() * 2 / 10 / 52:,.2f}")
