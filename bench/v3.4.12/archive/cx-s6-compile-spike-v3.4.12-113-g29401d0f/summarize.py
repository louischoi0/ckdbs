#!/usr/bin/env python3
"""Reads cores1.json and cores2.json, prints the per-cell table (median of the rounds, and the spread)."""
import json, statistics, sys
here = sys.argv[1]
for cores in (1, 2):
    d = json.load(open(f"{here}/cores{cores}.json"))
    print(f"\ncores = {cores}")
    print("cell | ops/round x rounds | client p0 | p25 | p50 | p95 | p99 (medians of rounds, us) | compile us/call (median, min-max) | server total us/call | compile/client p50 % (median, min-max) | compile/server total %")
    for c in d["cells"]:
        R = c["rounds"]
        m = lambda k: statistics.median(r[k] for r in R)
        shares = [100 * r["compile_us_per_call"] / r["p50"] for r in R]
        comp = [r["compile_us_per_call"] for r in R]
        srv = m("server_total_us_per_call")
        print(f"{c['cell']} | {R[0]['ops']}x{len(R)} | {m('p0'):.1f} | {m('p25'):.1f} | {m('p50'):.1f} | {m('p95'):.1f} | {m('p99'):.1f} | "
              f"{statistics.median(comp):.2f} ({min(comp):.2f}-{max(comp):.2f}) | {srv:.2f} | "
              f"{statistics.median(shares):.1f} ({min(shares):.1f}-{max(shares):.1f}) | {100 * statistics.median(comp) / srv:.1f}")
