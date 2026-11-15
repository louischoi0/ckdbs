#!/usr/bin/env python3
"""Markdown tables for the results file from full.json."""
import json, statistics as st, sys
r = json.load(open(sys.argv[1]))
c = {}
for x in r:
    c.setdefault((x['cell'], x['rows'], x['shape']), {}).setdefault(x['bin'], []).append(x)
m = st.median
for cell in (1, 2):
    print(f"\n#### Cell {cell}: server CPU per statement\n")
    print("| Shape | Rows | Stmts per run per arm | A CPU, median [min, max] over 10 runs | A2 (control) | B | B minus A | A2 minus A (noise) | Runs with B < A |")
    print("|---|---|---|---|---|---|---|---|---|")
    for k, d in c.items():
        if k[0] != cell:
            continue
        cp = {l: [x['server_cpu_us'] for x in d[l]] for l in d}
        a = m(cp['A'])
        f = lambda l: "%.2f µs [%.2f, %.2f]" % (m(cp[l]), min(cp[l]), max(cp[l]))
        dB = m(cp['B']) - a
        dC = m(cp['A2']) - a
        n = sum(1 for p, q in zip(cp['A'], cp['B']) if q < p)
        print(f"| {k[2]} | {k[1]:,} | {d['A'][0]['ops']:,} | {f('A')} | {f('A2')} | {f('B')} | {dB:+.2f} µs ({100*dB/a:+.1f} %, {dB*1000/k[1]:+.1f} ns/row) | {dC:+.2f} µs ({100*dC/a:+.1f} %) | {n} / 10 |")
    print(f"\n#### Cell {cell}: client-observed latency and throughput (median over 10 runs; throughput derived as 1,000,000 / mean µs)\n")
    print("| Shape | Arm | p0 | p25 | p50 | p75 | p99 | Throughput |")
    print("|---|---|---|---|---|---|---|---|")
    for k, d in c.items():
        if k[0] != cell:
            continue
        for l in ('A', 'B'):
            f = lambda key: m([x[key] for x in d[l]])
            print(f"| {k[2]} | {l} | {f('p0_us'):.1f} µs | {f('p25_us'):.1f} µs | {f('p50_us'):.1f} µs | {f('p75_us'):.1f} µs | {f('p99_us'):.1f} µs | {1e6/f('mean_us'):,.0f} QPS |")
