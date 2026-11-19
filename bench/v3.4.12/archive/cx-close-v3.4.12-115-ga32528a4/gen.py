#!/usr/bin/env python3
"""Markdown tables from an out*.json: python3 gen.py out.json base,control"""
import json, sys, statistics, collections
out = json.load(open(sys.argv[1]))
base, ctl = sys.argv[2].split(",")
d = collections.defaultdict(list)
for r in out: d[(r["cell"], r["engine"])].append(r)
cells = list(dict.fromkeys(r["cell"] for r in out))
engs = list(dict.fromkeys(r["engine"] for r in out))
med = lambda rs, k: statistics.median(x[k] for x in rs)
print("| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |")
print("|---|---|---|---|---|---|---|---|---|---|")
for c in cells:
    for e in engs:
        rs = d[(c, e)]
        print(f'| {c} | {e} | {sum(x["ops"] for x in rs):,} | {med(rs,"qps"):,.0f} qps | {med(rs,"p0"):.1f} µs | {med(rs,"p25"):.1f} µs | {med(rs,"p50"):.1f} µs | {med(rs,"p95"):.1f} µs | {med(rs,"p99"):.1f} µs | {med(rs,"server_cpu_us"):.1f} µs |')
print()
print(f"| Cell | Arm vs {base}: QPS | p50 | p99 | Server CPU | Control {ctl} vs {base}: QPS | p50 | CPU |")
print("|---|---|---|---|---|---|---|---|")
for c in cells:
    b = d[(c, base)]; k = d[(c, ctl)]
    for e in engs:
        if e in (base, ctl): continue
        rs = d[(c, e)]
        print(f'| {c} | {e}: {(med(rs,"qps")/med(b,"qps")-1)*100:+.1f} % | {med(rs,"p50")-med(b,"p50"):+.1f} µs | {med(rs,"p99")-med(b,"p99"):+.1f} µs | {med(rs,"server_cpu_us")-med(b,"server_cpu_us"):+.2f} µs | {(med(k,"qps")/med(b,"qps")-1)*100:+.1f} % | {med(k,"p50")-med(b,"p50"):+.1f} µs | {med(k,"server_cpu_us")-med(b,"server_cpu_us"):+.2f} µs |')
