import csv
import glob
import math
import os
import sys
from collections import defaultdict

MODE_LABELS = {
    0: "Baseline_FullSession",
    1: "All_5_Session_Switches",
    2: "PreMkt+NY_Open_Switch(07:30-10:30)",
    3: "NY_PM_Handoff_Switch(13:15-14:45)",
    4: "NY_Open+NY_PM_Switches(09:30-10:30,13:15-14:45)",
    5: "Asian+London_Switches(18:00-20:15,02:00-04:15)",
}


def pearson_corr(a: dict[int, float], b: dict[int, float], total_days: int) -> float:
    keys = set(a.keys()) | set(b.keys())
    if not keys or total_days <= 1:
        return 0.0
    n = float(total_days)
    sa = sum(a.values())
    sb = sum(b.values())
    ma = sa / n
    mb = sb / n
    cov = 0.0
    va = (n - len(keys)) * (ma * ma)
    vb = (n - len(keys)) * (mb * mb)
    for k in keys:
        da = a.get(k, 0.0) - ma
        db = b.get(k, 0.0) - mb
        cov += da * db
        va += da * da
        vb += db * db
    # Add zero-day cross terms
    zero_days = n - len(keys)
    cov += zero_days * (-ma) * (-mb)
    denom = math.sqrt(max(0.0, va) * max(0.0, vb))
    return (cov / denom) if denom > 1e-12 else 0.0


def main():
    in_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    shard_files = sorted(glob.glob(os.path.join(in_dir, "**", "session_switch_shard_*.csv"), recursive=True))
    day_files = sorted(glob.glob(os.path.join(in_dir, "**", "session_switch_days_*.csv"), recursive=True))

    rows = []
    for f in shard_files:
        with open(f, newline="") as fp:
            for r in csv.DictReader(fp):
                for k in r:
                    if k in ("uid", "asset"):
                        continue
                    if k in ("base_ny_trds", "base24_trds", "best_sw_mode", "sw_trds", "sw_m_safe_2k", "sw_m_sp500", "sw_all5_trds"):
                        r[k] = int(float(r[k]))
                    else:
                        r[k] = float(r[k])
                rows.append(r)

    days_map = defaultdict(dict)
    max_day_idx = 2520
    for f in day_files:
        with open(f, newline="") as fp:
            for r in csv.DictReader(fp):
                d_idx = int(r["cme_day_idx"])
                max_day_idx = max(max_day_idx, d_idx + 1)
                days_map[(r["uid"], r["asset"])][d_idx] = float(r["day_pnl"])

    if not rows:
        print("ERROR: No shard CSV rows found.")
        return

    sp500_net = rows[0]["sp500_net_50k"]
    sp500_dd = rows[0]["sp500_dd_50k"]

    active_pairs = 0
    improved_pf_cnt = 0
    reduced_dd_cnt = 0
    sum_base_dd = 0.0
    sum_sw_dd = 0.0
    base_ny_prof = base_24_prof = sw_all5_prof = sw_best_prof = 0
    base_ny_split = base_24_split = sw_best_split = 0
    newly_emerged = []
    beat_sp500_2k = 0
    beat_sp500_eq = 0
    winners_by_asset = defaultdict(list)

    for r in rows:
        if r["base_ny_trds"] >= 20 and r["sw_trds"] >= 20:
            active_pairs += 1
            sum_base_dd += r["base_ny_dd5m"]
            sum_sw_dd += r["sw_dd5m"]
            if r["sw_pf"] > r["base_ny_pf"]:
                improved_pf_cnt += 1
            if r["sw_dd5m"] < r["base_ny_dd5m"]:
                reduced_dd_cnt += 1

        b_ny_ok = r["base_ny_trds"] >= 25 and r["base_ny_net5m"] > 0 and r["base_ny_pf"] >= 1.10
        b_24_ok = r["base24_trds"] >= 25 and r["base24_net5m"] > 0 and r["base24_pf"] >= 1.10
        sw5_ok = r["sw_all5_trds"] >= 25 and r["sw_all5_net5m"] > 0 and r["sw_all5_pf"] >= 1.10
        sw_ok = r["sw_trds"] >= 25 and r["sw_net5m"] > 0 and r["sw_pf"] >= 1.15

        b_ny_sp = b_ny_ok and r["base_ny_h1"] > 0 and r["base_ny_h2"] > 0
        b_24_sp = b_24_ok and r["base24_h1"] > 0 and r["base24_h2"] > 0
        sw_sp = sw_ok and r["sw_h1"] > 0 and r["sw_h2"] > 0

        if b_ny_ok: base_ny_prof += 1
        if b_24_ok: base_24_prof += 1
        if sw5_ok: sw_all5_prof += 1
        if sw_ok: sw_best_prof += 1

        if b_ny_sp: base_ny_split += 1
        if b_24_sp: base_24_split += 1
        if sw_sp: sw_best_split += 1

        if not b_ny_sp and not b_24_sp and sw_sp:
            newly_emerged.append(r)

        if sw_sp:
            if r["sw_net_2k"] >= sp500_net:
                beat_sp500_2k += 1
            if r["sw_net_sp500"] >= sp500_net:
                beat_sp500_eq += 1
            winners_by_asset[r["asset"]].append(r)

    newly_emerged.sort(key=lambda x: x["sw_net_2k"], reverse=True)
    for a in winners_by_asset:
        winners_by_asset[a].sort(key=lambda x: x["sw_net_2k"], reverse=True)

    print("=" * 80)
    print(" 1. WHAT HAPPENS WHEN WE FILTER ALL 647 STRATEGIES TO SESSION-SWITCH WINDOWS?")
    print("=" * 80)
    print(f"  Total Pairs Evaluated: {len(rows)} | Active Pairs (>=20 trades): {active_pairs}")
    print(f"  Pairs Whose Profit Factor INCREASED in Switch Filter: {improved_pf_cnt} / {active_pairs} ({100.0*improved_pf_cnt/max(1,active_pairs):.1f}%)")
    print(f"  Pairs Whose Max Drawdown DECREASED in Switch Filter:  {reduced_dd_cnt} / {active_pairs} ({100.0*reduced_dd_cnt/max(1,active_pairs):.1f}%)")
    print(f"  Mean Max Drawdown (5 Micros): Baseline ${sum_base_dd/max(1,active_pairs):,.0f}  --->  Session-Switch ${sum_sw_dd/max(1,active_pairs):,.0f}")
    print("  Profitable Pairs (Trades>=25):")
    print(f"    - Baseline NY_All (09:30-16:00 All Day):      {base_ny_prof} (Split-Half H1>0 & H2>0: {base_ny_split})")
    print(f"    - Baseline 24-Hour All Sessions:              {base_24_prof} (Split-Half H1>0 & H2>0: {base_24_split})")
    print(f"    - Session-Switch Windows Filtered (PF>=1.15): {sw_best_prof} (Split-Half H1>0 & H2>0: {sw_best_split})")
    print(f"  BRAND-NEW Split-Half Robust Winners Emerging ONLY in Session-Switch Windows: {len(newly_emerged)} pairs!")
    print(f"  Solo Strategies Beating 10-Yr S&P 500 (+${sp500_net:,.0f} on $50k) on ONE Instrument:")
    print(f"    - Even while capped at Topstep $1,800 MaxDD:      {beat_sp500_2k} single strategies")
    print(f"    - At Equal Max Drawdown to S&P 500 (-${sp500_dd:,.0f} DD): {beat_sp500_eq} single strategies!")

    print("\n" + "=" * 80)
    print(" 2. TOP 25 BRAND-NEW STRATEGIES THAT EMERGE ONLY IN SESSION-SWITCH WINDOWS")
    print("=" * 80)
    print(f"{'Strategy_UID':38s} {'Sym':3s} {'BaseNet':>9s} {'BasePF':>7s} {'SwTrds':>7s} {'SwWR%':>7s} {'SwPF':>6s} {'SwNet5M$':>10s} {'SwDD5M$':>9s} {'Net@1.8kDD':>11s} {'Net@SP500DD':>12s}  Switch_Window")
    print("-" * 142)
    for r in newly_emerged[:25]:
        print(f"{r['uid'][:37]:38s} {r['asset']:3s} {r['base_ny_net5m']:9.0f} {r['base_ny_pf']:7.2f} {r['sw_trds']:7d} {r['sw_wr']:6.1f}% {r['sw_pf']:6.2f} {r['sw_net5m']:10.0f} {r['sw_dd5m']:9.0f} {r['sw_net_2k']:11.0f} {r['sw_net_sp500']:12.0f}  {MODE_LABELS.get(r['best_sw_mode'], 'Custom')}")

    watch = {
        "ig91_059_060_session_switch_v1",
        "ig91_059_060_session_switch_v2",
        "ig91_016_017_asia_sweep_fvg",
        "ig91_088_089_london_liquidity_sweep",
        "ig91_037_8am_candle_range",
        "ig91_039_8am_ote_po3",
        "ig91_045_first_hour_trend_break",
        "ig91_052_9am_crt_1m_fvg",
        "ig91_066_premarket_hl_breakout",
        "ig91_071_5min_orb_premarket_filter",
        "ig91_063_williams_r_ema_vwap",
    }
    print("\n" + "=" * 80)
    print(" 3. SPECIFIC IG91 SESSION-SWITCH & PRE-MARKET REELS: ALL-DAY VS. SWITCH FILTER")
    print("=" * 80)
    for r in rows:
        if r["uid"] in watch:
            print(f"{r['uid'][:34]:35s} {r['asset']:3s} | BaseNY: {r['base_ny_trds']:4d}t PF {r['base_ny_pf']:4.2f} ${r['base_ny_net5m']:8.0f} (DD ${r['base_ny_dd5m']:6.0f}) | Switch: {r['sw_trds']:4d}t PF {r['sw_pf']:4.2f} ${r['sw_net5m']:8.0f} (DD ${r['sw_dd5m']:6.0f}) [{MODE_LABELS.get(r['best_sw_mode'], '')}]")

    print("\n" + "=" * 80)
    print(f" 4. SINGLE-INSTRUMENT 10-YEAR BENCHMARK VS. S&P 500 (+${sp500_net:,.0f} Net, -${sp500_dd:,.0f} MaxDD)")
    print("=" * 80)
    for sym in ("NQ", "ES", "YM"):
        wvec = winners_by_asset[sym]
        ortho = []
        for cand in wvec:
            if cand["sw_pf"] < 1.25:
                continue
            cd = days_map.get((cand["uid"], cand["asset"]))
            if not cd:
                continue
            if any(abs(pearson_corr(cd, days_map[(s["uid"], s["asset"])], max_day_idx)) > 0.20 for s in ortho):
                continue
            ortho.append(cand)
            if len(ortho) >= 10:
                break

        print(f"\n--- INSTRUMENT: {sym} (Top Orthogonal Session-Switch Strategies on {sym} Alone) ---")
        print(f"{'Strategy_UID':38s} {'Trds':>6s} {'WR%':>7s} {'PF':>6s} {'Net5M$':>10s} {'DD5M$':>8s} {'Net@1.8kDD$':>12s} {'Net@SP500DD$':>13s}  BeatsS&P?")
        print("-" * 112)
        for r in ortho[:8]:
            b2 = r["sw_net_2k"] >= sp500_net
            bsp = r["sw_net_sp500"] >= sp500_net
            tag = "YES(@1.8k!)" if b2 else ("YES(@EqDD)" if bsp else "NO")
            print(f"{r['uid'][:37]:38s} {r['sw_trds']:6d} {r['sw_wr']:6.1f}% {r['sw_pf']:6.2f} {r['sw_net5m']:10.0f} {r['sw_dd5m']:8.0f} {r['sw_net_2k']:12.0f} {r['sw_net_sp500']:13.0f}  {tag}")

        if ortho:
            book_day = defaultdict(float)
            tot_trds = 0
            for s in ortho:
                tot_trds += s["sw_trds"]
                w = s["sw_m_safe_2k"] / 5.0
                for d_idx, dp in days_map[(s["uid"], s["asset"])].items():
                    book_day[d_idx] += dp * w
            eq = peak = dd = gw = gl = 0.0
            for d_idx in range(max_day_idx):
                dp = book_day.get(d_idx, 0.0)
                eq += dp
                peak = max(peak, eq)
                dd = max(dd, peak - eq)
                if dp > 0: gw += dp
                elif dp < 0: gl += abs(dp)
            s2k = (1800.0 / dd) if dd > 1.0 else 1.0
            ssp = (sp500_dd / dd) if dd > 1.0 else 1.0
            pf_book = (gw / gl) if gl > 0 else 99.0
            tag = "YES(@1.8k!)" if (eq * s2k >= sp500_net) else ("YES(@EqDD!)" if (eq * ssp >= sp500_net) else "NO")
            print("-" * 112)
            print(f"{f'COMBINED {sym}-ONLY SWITCH BOOK ({len(ortho)} legs)':38s} {tot_trds:6d} {'Day':>7s} {pf_book:6.2f} {eq:10.0f} {dd:8.0f} {eq*s2k:12.0f} {eq*ssp:13.0f}  {tag}")


if __name__ == "__main__":
    main()
