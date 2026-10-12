#!/usr/bin/env python3
"""M0 verdict. usage: verdict_m0.py <out/m0 dir>   (reads analysis.json from analyze_m0.py over every cell)
A cell (cores, shape, rows, metric) is INSIDE the noise when the 95 % CI of B - A contains zero, or overlaps the 95 % CI of the
A/A cell (the same binary on both ports) of the same cores, shape, rows and metric; RESOLVED otherwise. SHOW META is excluded."""
import json, sys

OUT = sys.argv[1]
res = json.load(open(f"{OUT}/analysis.json"))
ARMS = ["iso", "insert", "insert-again", "select-pk", "update-pk"]
DESC = {"iso": "SET ISOLATION LEVEL (control)", "insert": "INSERT", "insert-again": "INSERT again (replicate)",
        "select-pk": "SELECT by pk", "update-pk": "UPDATE by pk"}
METRICS = [("p50", "p50", "µs"), ("p99", "p99", "µs"), ("qps", "QPS", "/s"), ("cpu", "server CPU per statement", "µs")]
inside = {m[0]: 0 for m in METRICS}
resolved = {m[0]: [] for m in METRICS}
nonzero_ci = {m[0]: 0 for m in METRICS}
total = 0
for cores in ("c1", "c4"):
    for arm in ARMS:
        for rows in (200, 1000, 10000):
            r = res.get(f"relaxed-{cores}|{arm}|{rows}")
            a = res.get(f"aa-{cores}|{arm}|{rows}")
            if not r:
                continue
            total += 1
            for key, name, unit in METRICS:
                m, (lo, hi) = r[key][0], r[key][1]
                zero = lo <= 0 <= hi
                ov = False
                if a:
                    alo, ahi = a[key][1]
                    ov = not (hi < alo or lo > ahi)
                if zero or ov:
                    inside[key] += 1
                else:
                    resolved[key].append((cores, DESC[arm], rows, m, lo, hi, a[key][0] if a else None, a[key][1] if a else None))
print(f"relaxed cells: {total} (shape x rows x cores)")
for key, name, unit in METRICS:
    print(f"- {name}: {inside[key]} inside the noise, {len(resolved[key])} resolved")
    for c, d, rows, m, lo, hi, am, aci in resolved[key]:
        extra = f"; A/A {am:+.2f} [{aci[0]:+.2f}, {aci[1]:+.2f}]" if am is not None else "; no A/A cell"
        print(f"    - cores {c[1]}, {d}, {rows:,} rows: B - A {m:+.2f} {unit} [{lo:+.2f}, {hi:+.2f}]{extra}")
