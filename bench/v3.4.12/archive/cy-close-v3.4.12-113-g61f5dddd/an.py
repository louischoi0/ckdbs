#!/usr/bin/env python3
"""Aggregate a cy_close.py JSON: per (cell, engine) the median over rounds of each percentile,
qps and server CPU, with min..max of p50 across rounds. Markdown to stdout.
usage: an.py out.json [baseline-label]  (delta columns against the baseline label)"""
import json, statistics, sys
rows = json.load(open(sys.argv[1]))
base = sys.argv[2] if len(sys.argv) > 2 else None
cells, engs = [], []
for r in rows:
    if r["cell"] not in cells: cells.append(r["cell"])
    if r["engine"] not in engs: engs.append(r["engine"])
med = lambda xs: statistics.median(xs)
print("| cell | engine | rounds | ops/round | p0 us | p25 us | p50 us | p75 us | p90 us | p99 us | QPS | server CPU us/stmt | p50 min..max | p50 vs " + (base or "-") + " | p99 vs " + (base or "-") + " |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for c in cells:
    ref = None
    if base:
        ref = [r for r in rows if r["cell"] == c and r["engine"] == base]
    for e in engs:
        rs = [r for r in rows if r["cell"] == c and r["engine"] == e]
        m = {k: med([r[k] for r in rs]) for k in ("p0", "p25", "p50", "p75", "p90", "p99", "qps", "server_cpu_us")}
        p50s = [r["p50"] for r in rs]
        d50 = d99 = "-"
        if ref and e != base:
            d50 = f"{(m['p50'] / med([r['p50'] for r in ref]) - 1) * 100:+.1f}%"
            d99 = f"{(m['p99'] / med([r['p99'] for r in ref]) - 1) * 100:+.1f}%"
        print(f"| {c} | {e} | {len(rs)} | {rs[0]['ops']} | {m['p0']:.0f} | {m['p25']:.0f} | {m['p50']:.0f} | {m['p75']:.0f} | {m['p90']:.0f} | {m['p99']:.0f} | {m['qps']:.0f} | {m['server_cpu_us']:.1f} | {min(p50s):.0f}..{max(p50s):.0f} | {d50} | {d99} |")
