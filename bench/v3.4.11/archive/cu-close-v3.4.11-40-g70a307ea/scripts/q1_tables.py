#!/usr/bin/env python3
"""Markdown tables for the CU close results file, question 1 and the writes,
from q1.json (tools/cu_close_overhead.py) or rows.json (tools/cu_close_rows.py).

In the every-cell table p25 to p99 and the client mean are means of the
per-run values, p0 is the smallest unit of any run and max the largest."""
import json, sys, statistics as st
from collections import defaultdict

r = json.load(open(sys.argv[1]))
if isinstance(r, dict):
    r = r["cells"]
OFF = ["select-pk", "select-range", "select-idx", "select-scan", "select-join",
       "update-pk", "update-scan", "ins-del", "scan-in2", "scan-or", "pk-trail", "update-in2"]
MOVED = ["update-conj", "update-in4", "update-in64", "ins-del-in4"]
d = defaultdict(lambda: defaultdict(list))
for x in r:
    d[(x["shape"], x["rows"])][x["bin"]].append(x)
order = lambda kv: ((OFF + MOVED).index(kv[0][0]), kv[0][1])
runs = max(x["run"] for x in r) + 1


def se(xs):
    return st.stdev(xs) / len(xs) ** 0.5 if len(xs) > 1 else float("nan")


def cpu(sh, n, lab):
    return [x["server_cpu_us"] for x in sorted(d[(sh, n)][lab], key=lambda x: x["run"])]


print(f"runs: {runs}; loads per cell: min {min(float(x['load']) for x in r)} max {max(float(x['load']) for x in r)}; "
      f"bad units: {sum(x['bad'] for x in r)}\n")
print("### Off the set's shape: paired deltas, server CPU per unit\n")
print("| shape | rows | units per run | A | B - A (SE) | A2 - A (SE) | B - A % | reading |")
print("|---|---|---|---|---|---|---|---|")
pool = defaultdict(lambda: {"ba": [], "a2": [], "a": []})
for (sh, n), arms in sorted(d.items(), key=order):
    if sh not in OFF:
        continue
    a, b, a2 = cpu(sh, n, "A"), cpu(sh, n, "B"), cpu(sh, n, "A2")
    ba = [y - x for x, y in zip(a, b)]
    aa = [y - x for x, y in zip(a, a2)]
    pool[sh]["ba"] += ba; pool[sh]["a2"] += aa; pool[sh]["a"] += a
    m, s, m2, s2 = st.mean(ba), se(ba), st.mean(aa), se(aa)
    floor = max(2 * s, abs(m2) + 2 * s2)
    reading = "inside the floor" if abs(m) <= floor else ("B slower" if m > 0 else "B faster")
    print(f"| {sh} | {n:,} | {arms['A'][0]['ops']:,} | {st.mean(a):.2f} us | {m:+.2f} us ({s:.2f}) | {m2:+.2f} us ({s2:.2f}) | "
          f"{m / st.mean(a) * 100:+.1f} % | {reading} |")
print("\n### Off the set's shape: pooled over the sizes\n")
print("| shape | paired run-cells | A | B - A (SE) | A2 - A (SE) | B - A % | 2 SE % | largest cost not excluded, B - A + 2 SE |")
print("|---|---|---|---|---|---|---|---|")
for sh in OFF:
    p = pool[sh]
    if not p["ba"]:
        continue
    m, e, am = st.mean(p["ba"]), se(p["ba"]), st.mean(p["a"])
    print(f"| {sh} | {len(p['ba'])} | {am:.2f} us | {m:+.2f} us ({e:.2f}) | "
          f"{st.mean(p['a2']):+.2f} us ({se(p['a2']):.2f}) | {m / am * 100:+.1f} % | {2 * e / am * 100:.1f} % | "
          f"{m + 2 * e:+.2f} us, {(m + 2 * e) / am * 100:+.1f} % |")

print("\n### The shapes CU moved: server CPU per unit\n")
print("| shape | rows | units per run | A | A2 | B | A / B | B - A (SE) |")
print("|---|---|---|---|---|---|---|---|")
for (sh, n), arms in sorted(d.items(), key=order):
    if sh not in MOVED:
        continue
    a, b, a2 = cpu(sh, n, "A"), cpu(sh, n, "B"), cpu(sh, n, "A2")
    ba = [y - x for x, y in zip(a, b)]
    print(f"| {sh} | {n:,} | {arms['A'][0]['ops']:,} | {st.mean(a):,.1f} us | {st.mean(a2):,.1f} us | {st.mean(b):,.1f} us | "
          f"{st.mean(a) / st.mean(b):,.1f} times | {st.mean(ba):+,.1f} us ({se(ba):,.1f}) |")

print("\n### Every cell: server CPU and client latency (microseconds)\n")
print("| shape | rows | arm | units/run | runs | server CPU per unit (mean of run means) | min..max over runs | client mean | p0 | p25 | p50 | p75 | p90 | p99 | max | bad |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for (sh, n), arms in sorted(d.items(), key=order):
    for lab in ("A", "B", "A2"):
        xs = arms[lab]
        c = [x["server_cpu_us"] for x in xs]
        f = lambda k: st.mean(x[k] for x in xs)
        print(f"| {sh} | {n:,} | {lab} | {xs[0]['ops']:,} | {len(xs)} | {st.mean(c):,.2f} us | {min(c):,.2f}..{max(c):,.2f} us | "
              f"{f('mean_us'):,.1f} us | {min(x['p0_us'] for x in xs):,.1f} us | {f('p25_us'):,.1f} us | {f('p50_us'):,.1f} us | "
              f"{f('p75_us'):,.1f} us | {f('p90_us'):,.1f} us | {f('p99_us'):,.1f} us | {max(x['max_us'] for x in xs):,.1f} us | "
              f"{sum(x['bad'] for x in xs)} |")
