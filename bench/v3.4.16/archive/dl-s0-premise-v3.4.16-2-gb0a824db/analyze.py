#!/usr/bin/env python3
"""DL-S0 analysis: read raw.jsonl, print per-cell tables. Usage: analyze.py raw.jsonl"""
import json, sys, statistics as st
from collections import defaultdict

rows = [json.loads(l) for l in open(sys.argv[1])]
cells = defaultdict(lambda: defaultdict(list))
order = []
for r in rows:
    k = (r["cell"], r["cores"])
    if k not in order:
        order.append(k)
    cells[k][r["arm"]].append(r)

WAITS = ["optimizer", "cabin_stats", "cabin_partition", "stats_ring"]


def med(xs):
    return st.median(xs)


def waits(r, n):
    m = r["meta"]
    out = []
    for w in WAITS:
        c = m.get(f"contention_{w}_waits", 0)
        u = m.get(f"contention_{w}_wait_us", 0)
        out.append((c, u))
    return out


print("== per-arm results")
for k in order:
    for arm, rs in cells[k].items():
        cpu = [r["cpu_us_per_stmt"] for r in rs]
        q = [r["qps"] for r in rs]
        print(f"{k[0]:8s} c{k[1]} {arm:8s} n={len(rs)} cpu mean {st.mean(cpu):.2f} "
              f"({' '.join(f'{x:.2f}' for x in cpu)}) spread {max(cpu)-min(cpu):.2f} | "
              f"qps mean {st.mean(q):,.0f} spread {max(q)-min(q):,.0f} | stmts {sum(r['stmts'] for r in rs):,} "
              f"bad {sum(r['bad'] for r in rs)} busy {[r['busy'] for r in rs]} load {[r['load'] for r in rs]}")

print("\n== paired (first arm listed second minus first: B - A, A = first label of CELLS)")
for k in order:
    arms = list(cells[k])
    if len(arms) < 2:
        continue
    # canonical A = set/counted/reader/on ; B = unset/skipped/quiet/off
    a, b = arms[0], arms[1]
    ra = {r["round"]: r for r in cells[k][a]}
    rb = {r["round"]: r for r in cells[k][b]}
    rounds = sorted(set(ra) & set(rb))
    dc = [ra[i]["cpu_us_per_stmt"] - rb[i]["cpu_us_per_stmt"] for i in rounds]
    dq = [100 * (ra[i]["qps"] - rb[i]["qps"]) / rb[i]["qps"] for i in rounds]
    spread = max(
        max(r["cpu_us_per_stmt"] for r in cells[k][x]) - min(r["cpu_us_per_stmt"] for r in cells[k][x])
        for x in (a, b))
    print(f"{k[0]:8s} c{k[1]} {a} minus {b}: cpu delta {' '.join(f'{x:+.2f}' for x in dc)} mean {st.mean(dc):+.2f} us; "
          f"{a} slower (cpu) in {sum(1 for x in dc if x > 0)}/{len(dc)}; qps delta {' '.join(f'{x:+.1f}' for x in dq)} "
          f"mean {st.mean(dq):+.1f}%; max within-arm cpu spread {spread:.2f} us")

print("\n== latency (client us, median of per-lifetime percentiles)")
for k in order:
    for arm, rs in cells[k].items():
        p = {n: med([r[n] for r in rs]) for n in ("p0", "p25", "p50", "p90", "p99", "p999")}
        print(f"{k[0]:8s} c{k[1]} {arm:8s} n={sum(r['stmts'] for r in rs):,} "
              + " ".join(f"{n}={v:g}" for n, v in p.items())
              + f" pmax med={med([r['pmax'] for r in rs]):g}")

print("\n== waits per statement (count / us), mean over lifetimes")
for k in order:
    for arm, rs in cells[k].items():
        parts = []
        for i, w in enumerate(WAITS):
            cnt = st.mean([waits(r, 1)[i][0] / r["stmts"] for r in rs])
            us = st.mean([waits(r, 1)[i][1] / r["stmts"] for r in rs])
            parts.append(f"{w} {cnt:.4f} waits {us:.4f} us")
        print(f"{k[0]:8s} c{k[1]} {arm:8s} " + " | ".join(parts))

print("\n== other meta keys (sum of lifetimes, per arm)")
for k in order:
    for arm, rs in cells[k].items():
        keys = defaultdict(int)
        for r in rs:
            for kk, v in r["meta"].items():
                if not kk.startswith("contention_"):
                    keys[kk] += v
        print(f"{k[0]:8s} c{k[1]} {arm:8s} {dict(keys)}  reads={[r['reads'] for r in rs]}")
print("\n== all contention keys seen")
seen = defaultdict(int)
for r in rows:
    for kk, v in r["meta"].items():
        if kk.startswith("contention_"):
            seen[kk] += 1
print(dict(seen))
print("\n== notes")
for k in order:
    for arm, rs in cells[k].items():
        if rs[0]["note"]:
            print(k, arm, rs[0]["note"])
