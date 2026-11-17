#!/usr/bin/env python3
"""The close's list cells beside CU-S0's own numbers for the same cells.

Usage: s0cmp.py <s0 result.json> <s0 result-small.json> <s0 result-one.json>
                <close lists.json> <close small.json>

CU-S0's estimate of a key set is recomputed as its estimate.py did:
P(n) + max(0, J(rows, n) - J(1)), from CU-S0's files alone. The same-run
estimate is that formula over the close's own arms: P(n) is the walk (LA)
over the 1-row relation, J the close's J cells.
"""
import json, sys
from collections import defaultdict


def load(paths):
    cells = defaultdict(list)
    for path in paths:
        for c in json.load(open(path))["cells"]:
            cells[(c["rows"], c["col"], c["n"], c["arm"])].append(c["server_cpu_us_per_group"])
    return cells


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


s0, cl = load(sys.argv[1:4]), load(sys.argv[4:6])
walk0 = lambda k: mean(s0[k + ("L",)] + s0[k + ("L2",)])
set1 = lambda k: mean(cl[k + ("L",)] + cl[k + ("L2",)])


def estimate(rows, col, n):
    p = mean(s0[(1, col, n, "L")] + s0[(1, col, n, "L2")])
    j1 = mean([v for (r, c, m, a), vs in s0.items() if c == col and m == 1 and a == "J" for v in vs])
    return p + max(0.0, mean(s0[(rows, col, n, "J")]) - j1)


def estimate_now(rows, col, n):
    p = mean(cl[(1, col, n, "LA")])
    j1 = mean([v for (r, c, m, a), vs in cl.items() if c == col and m == 1 and a == "J" for v in vs])
    return p + max(0.0, mean(cl[(rows, col, n, "J")]) - j1)


print("| column | rows | n | walk at CU-S0 | walk now (LA) | now / then | set estimated at CU-S0 | set measured (L, L2) | "
      "measured / estimate | same-run estimate | measured / same-run | walk now / set | D at CU-S0 | D now | J at CU-S0 | J now |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
keys = sorted({k[:3] for k in cl if k[2] != 1 and k[0] != 1 and k[:3] + ("L",) in s0},
              key=lambda k: (k[1], k[0], k[2]))
for rows, col, n in keys:
    k = (rows, col, n)
    w0, w1, e, s = walk0(k), mean(cl[k + ("LA",)]), estimate(*k), set1(k)
    e1 = estimate_now(*k)
    print(f"| `{col}` | {rows:,} rows | {vals(n)} | {us(w0)} | {us(w1)} | {w1 / w0:.2f} | {us(e)} | {us(s)} | "
          f"{s / e:.2f} | {us(e1)} | {s / e1:.2f} | {w1 / s:,.2f} times | {us(mean(s0[k + ('D',)]))} | {us(mean(cl[k + ('D',)]))} | "
          f"{us(mean(s0[k + ('J',)]))} | {us(mean(cl[k + ('J',)]))} |")
missing = sorted({k[:3] for k in cl if k[2] != 1 and k[0] != 1 and k[:3] + ("L",) not in s0})
print(f"\ncells the close measured and CU-S0 did not: {missing}")
