#!/usr/bin/env python3
"""DG-S0's full percentile rows (p0, p25, p50, p95, p99, p99.9, max), the
median over runs of each, for the writers (from the schedule), the inserter
and the readers (from the schedule). Usage: dg_s0_pct.py [out-dir]"""
import glob
import json
import os
import statistics
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
Q = ["p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "p99.9_us", "max_us"]
cells = sorted({os.path.basename(p) for p in glob.glob(os.path.join(OUT, "r*", "*"))})
for key, title in (("writer_from_schedule", "one-row writers, from the schedule"),
                   ("inserter", "inserter, service"), ("read_from_schedule", "readers, from the schedule")):
    print(f"\n## {title}, µs (median over runs)\n")
    print("| cell | " + " | ".join(q[:-3] for q in Q) + " | n |")
    print("|" + "---|" * (len(Q) + 2))
    for c in cells:
        rs = [json.load(open(j)) for j in sorted(glob.glob(os.path.join(OUT, "r*", c, "*.json")))]
        rows = [r[key] for r in rs if r.get(key)]
        if not rows:
            continue
        vals = [f"{statistics.median(x[q] for x in rows):.0f}" for q in Q]
        print(f"| {c} | " + " | ".join(vals) + f" | {statistics.median(x['n'] for x in rows):.0f} |")
