#!/usr/bin/env python3
"""CE close supplement: the `drain` client path (no cell touched), A against B. usage: analyze_drain.py [RUN_DIR]"""
import json, os, statistics, sys

RUN = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
SHAPES = ["range-1", "range-100", "range-1000", "range-10000", "range-60480", "filter-sym3"]
ROWS = {"range-1": 1, "range-100": 100, "range-1000": 1000, "range-10000": 10000, "range-60480": 60480, "filter-sym3": 7560}
med = statistics.median


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    lo = int(k); hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def spr(xs):
    m = med(xs)
    return 100 * (max(xs) - min(xs)) / m


def us(x):
    return f"{x:,.1f} µs"


cells = {}
for b in "AB":
    for p in (1, 2, 3):
        fn = f"{RUN}/rowcost/drain-p{p}-{b}.jsonl"
        if os.path.exists(fn):
            for l in open(fn):
                c = json.loads(l)
                cells.setdefault((b, c["shape"]), []).append((p, c))
print("## Supplement: the `drain` client path (frame headers only; the wire and the server, no client work)\n")
print("Runs per binary: " + str({b: len(cells.get((b, 'range-1'), [])) for b in 'AB'}) + ". Wall = the client's per statement; server CPU from /proc/<pid>/stat (10 ms ticks).\n")
print("| shape | rows returned | statements per run | A wall, runs | B wall, runs | A median | B median | B over A | spread A / B | a finding? | A server CPU | B server CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for s in SHAPES:
    a = [c for _, c in sorted(cells.get(("A", s), []), key=lambda x: x[0])]
    b = [c for _, c in sorted(cells.get(("B", s), []), key=lambda x: x[0])]
    if not a or not b:
        continue
    wa, wb = [c["wall_us_per_stmt"] for c in a], [c["wall_us_per_stmt"] for c in b]
    d = 100 * (med(wb) - med(wa)) / med(wa)
    f = abs(d) > max(spr(wa), spr(wb))
    print(f"| {s} | {ROWS[s]:,} rows | {med(c['n'] for c in a):.0f} | " + " / ".join(f"{v:,.0f}" for v in wa) + " µs | " + " / ".join(f"{v:,.0f}" for v in wb)
          + f" µs | {us(med(wa))} | {us(med(wb))} | {d:+.1f} % | {spr(wa):.1f} % / {spr(wb):.1f} % | {'**yes**' if f else 'no (inside the spread)'} | "
          + f"{us(med(c['server_cpu_us_per_stmt'] for c in a))} | {us(med(c['server_cpu_us_per_stmt'] for c in b))} |")
print("\nDistribution of one statement (all statements of the runs pooled):\n")
print("| shape | rows returned | binary | statements | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|---|")
for s in SHAPES:
    for b in "AB":
        xs = [x for _, c in cells.get((b, s), []) for x in c["lat_us"]]
        if xs:
            print(f"| {s} | {ROWS[s]:,} rows | {b} | {len(xs):,} | " + " | ".join(us(pct(xs, p)) for p in (0, 25, 50, 95, 99)) + " |")
