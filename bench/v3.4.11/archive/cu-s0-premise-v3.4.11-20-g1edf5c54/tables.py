#!/usr/bin/env python3
"""Markdown tables for the CU-S0 results file, from out.json."""
import json, sys
from collections import defaultdict

o = json.load(open(sys.argv[1]))
cells = defaultdict(list)
for c in o["cells"]:
    cells[(c["rows"], c["col"], c["n"], c["arm"])].append(c)


def us(v):
    if v >= 1e6:
        return f"{v / 1e6:,.2f} s"
    if v >= 1e4:
        return f"{v / 1e3:,.1f} ms"
    return f"{v:,.0f} µs" if v >= 100 else f"{v:,.1f} µs"


def vals(n):
    return f"{n:,} value" + ("" if n == 1 else "s")


def mean(xs):
    return sum(xs) / len(xs)


def pooled(key, field):
    return mean([c[field] for c in cells[key]])


keys = sorted({k[:3] for k in cells}, key=lambda k: (k[1], k[0], k[2]))
print("## Server CPU per list\n")
print("| column | rows | n | L walk | L2 walk | spread | D n statements | J n descents | L / J | L / D | reading |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for rows, col, n in keys:
    k = (rows, col, n)
    ls = [c["server_cpu_us_per_group"] for a in ("L", "L2") for c in cells[k + (a,)]]
    L, L2 = pooled(k + ("L",), "server_cpu_us_per_group"), pooled(k + ("L2",), "server_cpu_us_per_group")
    D, J = pooled(k + ("D",), "server_cpu_us_per_group"), pooled(k + ("J",), "server_cpu_us_per_group")
    lm = mean(ls)
    spread = (max(ls) - min(ls)) / lm
    def verdict(x):
        d = (lm - x) / lm
        if abs(d) <= spread:
            return "inside noise"
        return "faster" if d > 0 else "slower"
    reading = f"J {verdict(J)}, D {verdict(D)}"
    print(f"| `{col}` | {rows:,} rows | {vals(n)} | {us(L)} | {us(L2)} | {spread * 100:.0f} % | {us(D)} | {us(J)} | "
          f"{lm / J:,.1f} times | {lm / D:,.2f} times | {reading} |")

print("\n## Latency per list, client side\n")
print("| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for rows, col, n in keys:
    for arm in ("L", "L2", "D", "J"):
        cs = cells[(rows, col, n, arm)]
        # percentiles are per repeat; report the repeat-mean of each, the extremes for p0 and max
        g = sum(c["groups"] for c in cs)
        f = lambda p: mean([c["lat"][p] for c in cs])
        print(f"| `{col}` | {rows:,} rows | {vals(n)} | {arm} | {g:,} groups | {mean([c['gps'] for c in cs]):,.1f} lists/s | "
              f"{us(min(c['lat']['p0_us'] for c in cs))} | {us(f('p25_us'))} | {us(f('p50_us'))} | {us(f('p75_us'))} | "
              f"{us(f('p99_us'))} | {us(max(c['lat']['max_us'] for c in cs))} | {cs[0]['rows_per_group']:,.0f} rows |")

print("\n## Per descent\n")
print("| column | rows | n | J CPU per value | D CPU per value | L CPU per row walked |")
print("|---|---|---|---|---|---|")
for rows, col, n in keys:
    k = (rows, col, n)
    J, D = pooled(k + ("J",), "server_cpu_us_per_group"), pooled(k + ("D",), "server_cpu_us_per_group")
    L = mean([c["server_cpu_us_per_group"] for a in ("L", "L2") for c in cells[k + (a,)]])
    print(f"| `{col}` | {rows:,} rows | {vals(n)} | {J / n:.2f} µs | {D / n:.2f} µs | " +
          ("no walk (one value is keyed) |" if n == 1 else f"{L / rows * 1000:,.0f} ns |"))

print("\n## Host, per arm run\n")
loads = [float(c["host"]["loadavg"].split()[0]) for c in o["cells"]]
cc = [int(c["host"]["cc1plus"] or 0) for c in o["cells"]]
print(f"load average (1 min) min {min(loads)} max {max(loads)}; cc1plus processes min {min(cc)} max {max(cc)}; "
      f"{len(o['cells'])} arm runs; checks all same_rows: {all(c['same_rows'] for c in o['checks'])} ({len(o['checks'])} cells)")
print("\n## Plans\n")
seen = set()
for p in o["plans"]:
    if p["n"] not in (4,) and not (p["arm"] == "L" and p["n"] == 1):
        continue
    key = (p["rows"], p["col"], p["n"], p["arm"])
    if key in seen or p["rows"] != max(x["rows"] for x in o["plans"]):
        continue
    seen.add(key)
    print(f"- `{p['col']}`, {p['rows']:,} rows, n = {p['n']}, arm {p['arm']}: `{p['sql'][:110]}`")
    for line in p["analyze"]:
        if line.startswith(("step", "  filter", "class", "analyze", "DB time")):
            print(f"    - `{line[:230]}`")
