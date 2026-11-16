#!/usr/bin/env python3
"""The key set estimate for the CU-S0 results file.

estimate(rows, n) = P(n) + max(0, J(rows, n) - J(1))

P(n)        the L arm over the 1-row relation: one statement carrying the
            n-item list, with next to nothing to walk
J(rows, n)  the n descents in one statement
J(1)        that statement's cost with one descent, which P(n) already
            carries as its own statement cost: the mean of the column's
            n = 1 J cells over every relation size that has one (1, 1,000
            and 100,000 rows; they differ by noise, not by size). The
            descent term is floored at 0.

Usage: estimate.py result.json result-small.json result-one.json
"""
import json, sys
from collections import defaultdict

cells = defaultdict(list)
for path in sys.argv[1:4]:
    for c in json.load(open(path))["cells"]:
        cells[(c["rows"], c["col"], c["n"], "L" if c["arm"] == "L2" else c["arm"])].append(
            c["server_cpu_us_per_group"])


def mean(xs):
    return sum(xs) / len(xs)


def us(v):
    if v >= 1e6:
        return f"{v / 1e6:,.2f} s"
    if v >= 1e4:
        return f"{v / 1e3:,.1f} ms"
    return f"{v:,.0f} µs" if v >= 100 else f"{v:,.1f} µs"


def vals(n):
    return f"{n:,} value" + ("" if n == 1 else "s")


print("| column | rows | n | L walk | spread | P list alone | descents, J - J(1) | key set estimate | L / estimate | reading |")
print("|---|---|---|---|---|---|---|---|---|---|")
keys = sorted({k[:3] for k in cells if k[0] != 1 and k[2] != 1}, key=lambda k: (k[1], k[0], k[2]))
for rows, col, n in keys:
    ls = cells[(rows, col, n, "L")]
    L = mean(ls)
    spread = (max(ls) - min(ls)) / L
    P = mean(cells[(1, col, n, "L")])
    j1 = [x for k, v in cells.items() if k[1] == col and k[2] == 1 and k[3] == "J" for x in v]
    desc = max(0.0, mean(cells[(rows, col, n, "J")]) - mean(j1))
    est = P + desc
    d = (L - est) / L
    reading = "inside spread" if abs(d) <= spread else ("set faster" if d > 0 else "set slower")
    print(f"| `{col}` | {rows:,} rows | {vals(n)} | {us(L)} | {spread * 100:.0f} % | {us(P)} | {us(desc)} | {us(est)} | "
          f"{L / est:,.2f} times | {reading} |")

print()
print("| column | n | P list alone (L over 1 row) | per item over one value's statement |")
print("|---|---|---|---|")
for col in ("id", "k"):
    p1 = mean(cells[(1, col, 1, "L")])
    for n in sorted({k[2] for k in cells if k[0] == 1}):
        P = mean(cells[(1, col, n, "L")])
        per = "the single-value statement" if n == 1 else f"{(P - p1) / (n - 1):.2f} µs"
        print(f"| `{col}` | {vals(n)} | {us(P)} | {per} |")
