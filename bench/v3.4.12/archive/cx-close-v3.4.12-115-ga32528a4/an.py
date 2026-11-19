#!/usr/bin/env python3
"""Median over rounds of each per-block statistic, per (cell, engine); deltas vs A and vs the A2-A floor."""
import json, sys, statistics, collections
out = json.load(open(sys.argv[1]))
ctl = sys.argv[2].split(",")  # engine order, first = baseline, second = control
d = collections.defaultdict(list)
for r in out: d[(r["cell"], r["engine"])].append(r)
cells = list(dict.fromkeys(r["cell"] for r in out))
engs = list(dict.fromkeys(r["engine"] for r in out))
med = lambda rs, k: statistics.median(x[k] for x in rs)
print("cell,engine,blocks,n,qps,p0,p25,p50,p95,p99,cpu,qps_min,qps_max,competing_blocks,maxload")
for c in cells:
    for e in engs:
        rs = d[(c, e)]
        comp = sum(1 for x in rs if x["host"]["competing"])
        ml = max(float(x["host"]["loadavg"].split()[0]) for x in rs)
        print(f'{c},{e},{len(rs)},{sum(x["ops"] for x in rs)},{med(rs,"qps"):.0f},{med(rs,"p0"):.1f},{med(rs,"p25"):.1f},{med(rs,"p50"):.1f},{med(rs,"p95"):.1f},{med(rs,"p99"):.1f},{med(rs,"server_cpu_us"):.1f},{min(x["qps"] for x in rs):.0f},{max(x["qps"] for x in rs):.0f},{comp},{ml}')
print()
print("deltas vs", ctl[0], "(qps %, p50 us, p99 us, cpu us)")
for c in cells:
    b = d[(c, ctl[0])]
    for e in engs:
        if e == ctl[0]: continue
        rs = d[(c, e)]
        print(f'{c},{e},{(med(rs,"qps")/med(b,"qps")-1)*100:+.1f}%,{med(rs,"p50")-med(b,"p50"):+.1f},{med(rs,"p99")-med(b,"p99"):+.1f},{med(rs,"server_cpu_us")-med(b,"server_cpu_us"):+.2f}')
