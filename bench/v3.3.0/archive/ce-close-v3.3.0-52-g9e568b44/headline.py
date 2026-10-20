#!/usr/bin/env python3
"""CE close headline: sweep throughput (QPS derived as 1,000,000 / wall per statement), A against B, send and split paths."""
import json, os, statistics, sys

RUN = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
SHAPES = ["range-1", "range-100", "range-1000", "range-10000", "range-60480", "filter-sym3"]
ROWS = {"range-1": 1, "range-100": 100, "range-1000": 1000, "range-10000": 10000, "range-60480": 60480, "filter-sym3": 7560}
med = statistics.median


def spr(xs):
    m = med(xs)
    return 100 * (max(xs) - min(xs)) / m


cells = {}
for b in "AB":
    for p in (1, 2, 3):
        for l in open(f"{RUN}/rowcost/cells-p{p}-{b}.jsonl"):
            c = json.loads(l)
            cells.setdefault((b, c["shape"], c["mode"]), []).append(c)
for m in ("send", "split"):
    print(f"`{m}` path. QPS is derived as 1,000,000 / wall per statement of each run; medians of 3 runs per binary (A, B alternate start order); spread = (max - min) / median.\n")
    print("| shape | rows returned | A QPS (3 runs) | B QPS (3 runs) | A median | B median | B over A (QPS) | spread A / B | outside the larger spread? |")
    print("|---|---|---|---|---|---|---|---|---|")
    for s in SHAPES:
        qa = [1e6 / c["wall_us_per_stmt"] for c in cells[("A", s, m)]]
        qb = [1e6 / c["wall_us_per_stmt"] for c in cells[("B", s, m)]]
        d = 100 * (med(qb) - med(qa)) / med(qa)
        sa, sb = spr(qa), spr(qb)
        fmt = (lambda v: f"{v:,.0f}") if med(qa) > 200 else ((lambda v: f"{v:,.2f}") if med(qa) < 10 else (lambda v: f"{v:,.1f}"))
        print(f"| {s} | {ROWS[s]:,} rows | " + " / ".join(fmt(v) for v in qa) + " qps | " + " / ".join(fmt(v) for v in qb)
              + f" qps | {fmt(med(qa))} qps | {fmt(med(qb))} qps | {d:+.1f} % | {sa:.1f} % / {sb:.1f} % | {'**yes**' if abs(d) > max(sa, sb) else 'no'} |")
    print()
