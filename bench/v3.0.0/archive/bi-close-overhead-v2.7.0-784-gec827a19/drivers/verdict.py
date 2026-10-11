#!/usr/bin/env python3
"""BI-Q10 (iii) reading of the p50 deltas. For every relaxed (cores, shape, rows) cell: B - A median and CI against the A/A cell
(same binary on both ports) of the same cores, shape and rows. A cell is INSIDE the noise when |median| <= 0.2 us (the BA close's
relaxed floor) or its CI overlaps the A/A CI of the same cell; OUTSIDE otherwise.  Synced cells: |median| against the replicate.
usage: verdict.py <out-dir>"""
import glob, json, random, statistics as st, sys

OUT = sys.argv[1]
SIZES = (200, 1000, 10000)
DESC = {"insert": "INSERT, pk omitted", "insert-again": "INSERT again (replicate)", "select-pk": "SELECT by pk", "update-pk": "UPDATE by pk",
        "delete-pk": "DELETE by pk", "bulk20": "bulk INSERT, 20 rows", "range100": "whole-relation walk", "ping2": "SHOW NAMESPACES (control)",
        "iso": "SET ISOLATION LEVEL (control)", "ping": "SHOW META"}
random.seed(1)


def boot(vals, n=4000):
    b = sorted(st.median(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


def cell(name, rows, arm):
    fs = glob.glob(f"{OUT}/{name}/n{rows}-r*.json")
    d = [json.load(open(f)) for f in fs]
    v = [s["arms"]["B"][arm]["p50_us"] - s["arms"]["A"][arm]["p50_us"] for s in d]
    return st.median(v), boot(v), len(v)


inside = outside = 0
rows_out = []
print("| cores | shape | rows | B - A p50 (95 % CI), 16 runs | A/A p50 delta (95 % CI), 8 runs | reading |")
print("|---|---|---|---|---|---|")
for c in (1, 2):
    for arm in ("insert", "insert-again", "select-pk", "update-pk", "delete-pk", "bulk20", "range100", "ping2", "iso", "ping"):
        for r in SIZES:
            m, ci, n = cell(f"relaxed-c{c}", r, arm)
            am, aci, an_ = cell(f"aa-c{c}", r, arm)
            ov = not (ci[1] < aci[0] or ci[0] > aci[1])
            small = abs(m) <= 0.2
            if arm == "ping":
                tag = "not an OLTP statement: B's reply carries 13 more fields"
            elif small or ov:
                tag = "inside" + (" (median within 0.2 µs)" if small else " (CI overlaps the A/A CI)")
                inside += 1
            else:
                tag = f"OUTSIDE: {'B faster' if m < 0 else 'B slower'}"
                outside += 1
                rows_out.append((c, DESC[arm], r, m, ci))
            print(f"| {c} | {DESC[arm]} | {r:,} | {m:+.2f} µs [{ci[0]:+.2f}, {ci[1]:+.2f}] | {am:+.2f} µs [{aci[0]:+.2f}, {aci[1]:+.2f}] | {tag} |")
print(f"\n**inside the noise: {inside} cells (SHOW META excluded); outside: {outside}**" + "".join(f" ({x[1]}, cores {x[0]}, {x[2]:,} rows, {x[3]:+.2f} µs [{x[4][0]:+.2f}, {x[4][1]:+.2f}])" for x in rows_out))
