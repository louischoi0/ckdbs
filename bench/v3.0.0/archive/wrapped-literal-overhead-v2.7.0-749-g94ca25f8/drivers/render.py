#!/usr/bin/env python3
"""render.py RESDIR -> markdown tables from analysis.json and pooled.json."""
import json, sys
R = sys.argv[1]
a = json.load(open(f"{R}/analysis.json"))["cells"]
p = json.load(open(f"{R}/pooled.json"))
arms = []
for k in a:
    n, arm = k.split("|")
    if arm not in arms:
        arms.append(arm)
sizes = sorted({int(k.split("|")[0]) for k in a})
print("### per-run medians and B-A deltas (median of per-run differences; IQR of those differences)")
print("| rows | arm | A p50 us | B p50 us | d p50 us (IQR) | B>A | d p99 us (IQR) | B>A | d mean us (IQR) | A QPS | B QPS | d QPS |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for n in sizes:
    for arm in arms:
        c = a.get(f"{n}|{arm}")
        if not c:
            continue
        d50, d99, dm = c["d_p50"], c["d_p99"], c["d_mean"]
        qa, qb = 1e6 / c["A_mean"], 1e6 / c["B_mean"]
        print(f"| {n} | {arm} | {c['A_p50']:.1f} | {c['B_p50']:.1f} | {d50['median']:+.2f} ({d50['q1']:+.2f}, {d50['q3']:+.2f}) | {d50['B_gt_A']}/{d50['n']} "
              f"| {d99['median']:+.1f} ({d99['q1']:+.1f}, {d99['q3']:+.1f}) | {d99['B_gt_A']}/{d99['n']} "
              f"| {dm['median']:+.2f} ({dm['q1']:+.2f}, {dm['q3']:+.2f}) | {qa:,.0f} | {qb:,.0f} | {qb-qa:+,.0f} |")
print()
print("### pooled over runs (all statements), us")
print("| rows | arm | engine | n | p0 | p25 | p50 | p75 | p99 |")
print("|---|---|---|---|---|---|---|---|---|")
for n in sizes:
    for arm in arms:
        for lab in ("A", "B"):
            e = p.get(f"{n}|{arm}|{lab}")
            if e:
                print(f"| {n} | {arm} | {lab} | {e['n']} | {e['p0']:.1f} | {e['p25']:.1f} | {e['p50']:.1f} | {e['p75']:.1f} | {e['p99']:.1f} |")
