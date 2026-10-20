#!/usr/bin/env python3
"""CE close: the overhead control (bn_census point statements), A against B. usage: analyze_census.py [RUN_DIR]"""
import json, os, statistics, sys

RUN = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
med = statistics.median


def spr(xs):
    m = med(xs)
    return 100 * (max(xs) - min(xs)) / m if m else 0.0


def us(x, d=2):
    return f"{x:,.{d}f} µs"


def vtxt(a, b):
    ma, mb = med(a), med(b)
    d = 100 * (mb - ma) / ma
    sa, sb = spr(a), spr(b)
    return f"{d:+.1f} %", f"{sa:.1f} % / {sb:.1f} %", "**yes**" if abs(d) > max(sa, sb) else "no (inside the spread)"


d = json.load(open(f"{RUN}/json/census.json"))
rec = {}
unclean = 0
total = 0
for r in d["runs"]:
    for a in r["arms"]:
        rec.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
        unclean += a["host"].get("clean") is False
        total += 1
print("## Overhead control: point statements, server CPU per statement (bn_census, A and B side by side)\n")
print(f"{len(d['runs'])} runs x 6 blocks, 1,998 statements per arm per run. Unclean cells: {unclean} of {total}. Server CPU = the sum of /proc/<pid>/task/*/schedstat over every server thread; spread = (max - min) / median over the runs.\n")
print("| shape | path | A server CPU | B server CPU | B over A | spread A / B | a finding? | A wall mean | B wall mean | wall B over A | wall spread A / B |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
order = ["noop", "ping", "select-pk", "update-lit"]
keys = sorted({(s, p) for (s, p, b) in rec}, key=lambda k: (order.index(k[0]), k[1]))
for s, p in keys:
    a_ = rec[(s, p, "A")]
    b_ = rec[(s, p, "B")]
    ca, cb = [x["server_cpu_us"] for x in a_], [x["server_cpu_us"] for x in b_]
    wa, wb = [x["mean_us"] for x in a_], [x["mean_us"] for x in b_]
    r, sp, f = vtxt(ca, cb)
    rw, spw, _ = vtxt(wa, wb)
    print(f"| {s} | {p} | {us(med(ca))} | {us(med(cb))} | {r} | {sp} | {f} | {us(med(wa))} | {us(med(wb))} | {rw} | {spw} |")
print("\nWall of one statement (each run's percentile, median over the runs; the driver reports p90, not p95):\n")
print("| shape | path | binary | statements per run | p0 | p25 | p50 | p90 | p99 |")
print("|---|---|---|---|---|---|---|---|---|")
for s, p in keys:
    for b in "AB":
        x = rec[(s, p, b)]
        print(f"| {s} | {p} | {b} | {x[0]['ops']:,} | " + " | ".join(us(med(q[k] for q in x)) for k in ("p0_us", "p25_us", "p50_us", "p90_us", "p99_us")) + " |")
