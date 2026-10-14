import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

REPS = ["", "r2-", "r3-"]
CELLS = ["c1-g", "c8-g", "c1-s"]
SHAPES = ["bar-lookup", "bar-range", "day-slice", "symbol-history", "cross-join", "point-join", "model-join"]
# PostgreSQL 18.6 standalone floor, reused from results-scenario1-backtest-pg18-v2.7.0-545-gf2f1ee7.md (36449d28), one run
PG = {"bar-lookup": (14834.0, 15286.8, None, None), "bar-range": (1382.1, 1374.7, None, None),
      "day-slice": (414.4, 401.3, 8892.4, 409.3), "symbol-history": (33.3, 32.3, 33.9, 32.3),
      "cross-join": (375.6, 385.5, 3982.7, 380.1), "point-join": (5769.2, 5790.4, None, None),
      "model-join": (983.2, 1000.5, 1099.4, 1122.4)}
PGW = {1: 658.7, 10: 4883.3, 100: 14048.4, 1000: 18756.6}
PGC = {1: 390.7, 2: 760.8, 4: 1432.3, 8: 1725.1}


def sweep(d):
    return {s["shape"]: s for s in d["meta"]["qps_sweep"]}


print("### unmodified driver (tools/scenario1_backtest.py), three cells")
for t in ("s1u-c1-g", "s1u-c8-g", "s1u-c1-s"):
    print(t, "exit", open(f"{L}/{t}.rc").read().strip())
    print(open(f"{L}/{t}.stdout").read())

print("### QPS sweep per shape and case, BTREE copy, per cell: three runs (cold / warm / cabin / dropped), median")
for c in CELLS:
    print(f"#### cell {c}")
    print("| shape | detail | cold | warm | cabin | dropped | cabin over warm | spread of warm (3 runs) |")
    print("|---|---|---|---|---|---|---|---|")
    for s in SHAPES:
        rows = [sweep(load(f"{r}s1b-{c}"))[s] for r in REPS]
        def med(case):
            v = [x[case]["qps"] for x in rows if x[case]]
            return statistics.median(v) if v else None
        warm = [x["warm"]["qps"] for x in rows]
        cab = med("cabin")
        fm = lambda x: "not applicable" if x is None else f"{x:,.1f} qps"
        print(f"| {s} | {rows[0]['detail']} | {fm(med('cold'))} | {fm(med('warm'))} | {fm(cab)} | {fm(med('dropped'))} | "
              f"{'-' if cab is None else '%.2fx' % (cab/med('warm'))} | {100*(max(warm)/min(warm)-1):.1f} % |")
    print()

print("### versus PostgreSQL 18.6 (reused), cell c1-g: KDS median over PG, cold / warm / (Cabin vs PG btree index) / dropped")
print("| shape | KDS cold | PG cold | KDS/PG cold | KDS warm | PG warm | KDS/PG warm | KDS Cabin | PG index built | KDS/PG indexed-or-cabin |")
print("|---|---|---|---|---|---|---|---|---|---|")
for s in SHAPES:
    rows = [sweep(load(f"{r}s1b-c1-g"))[s] for r in REPS]
    cold = statistics.median([x["cold"]["qps"] for x in rows])
    warm = statistics.median([x["warm"]["qps"] for x in rows])
    cab = [x["cabin"]["qps"] for x in rows if x["cabin"]]
    cab = statistics.median(cab) if cab else None
    p = PG[s]
    print(f"| {s} | {cold:,.1f} qps | {p[0]:,.1f} qps | {cold/p[0]:.2f}x | {warm:,.1f} qps | {p[1]:,.1f} qps | {warm/p[1]:.2f}x | "
          f"{'-' if cab is None else '%s qps' % format(cab, ',.1f')} | {'-' if p[2] is None else '%s qps' % format(p[2], ',.1f')} | "
          f"{'-' if cab is None or p[2] is None else '%.2fx' % (cab/p[2])} |")
print()

print("### write sweep (rows/s), median of three, per cell; PG reused")
print("| rows per BEGIN/COMMIT | c1-g | c8-g | c1-s | PG18 (reused, 1 run) |")
print("|---|---|---|---|---|")
for b in (1, 10, 100, 1000):
    row = []
    for c in CELLS:
        v = [[w for w in load(f"{r}s1b-{c}")["meta"]["write_sweep"] if w["batch"] == b][0]["qps"] for r in REPS]
        row.append(f"{statistics.median(v):,.1f} rows/s (spread {100*(max(v)/min(v)-1):.1f} %)")
    print(f"| {b} row{'s' if b > 1 else ''} | " + " | ".join(row) + f" | {PGW[b]:,.1f} rows/s |")
print()
print("### concurrency sweep (q/s of the backtest join), median of three, per cell; PG reused")
print("| connections | c1-g | c8-g | c1-s | PG18 (reused, 1 run) |")
print("|---|---|---|---|---|")
for n in (1, 2, 4, 8):
    row = []
    for c in CELLS:
        v = [[w for w in load(f"{r}s1b-{c}")["meta"]["concurrency_sweep"] if w["connections"] == n][0]["qps"] for r in REPS]
        row.append(f"{statistics.median(v):,.1f} qps (spread {100*(max(v)/min(v)-1):.1f} %)")
    print(f"| {n} | " + " | ".join(row) + f" | {PGC[n]:,.1f} qps |")
print()

print("### percentiles, all phases, run 1 of each cell")
print("| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |")
print("|---|---|---|---|---|---|---|---|---|---|")
for c in CELLS:
    d = load(f"s1b-{c}")
    for p in d["phases"]:
        print(f"| `s1b-{c}` | {p['phase']} | {pct_row(p)} |")
print()

print("### run-to-run: elapsed-phase means of load-bars, backtest-read, result-insert (qps = 1e6/mean), three runs")
for c in CELLS:
    for name in ("load-bars", "load-stats", "backtest-read", "result-insert", "read-bar-lookup", "read-day-slice", "agg-by-session"):
        v = [qps_from_mean(ph(load(f"{r}s1b-{c}"), name)) for r in REPS]
        print(f"| s1b-{c} | {name} | " + " | ".join(f"{x:,.1f} qps" for x in v) + f" | median {statistics.median(v):,.1f} qps | spread {100*(max(v)/min(v)-1):.1f} % |")
print()
print("### verify and exits")
for r in REPS:
    for c in CELLS:
        t = f"{r}s1b-{c}"
        d = load(t)
        print(t, "exit", open(f"{L}/{t}.rc").read().strip(), "verify_problems", d["meta"]["verify_problems"], "errors", sum(p["errors"] for p in d["phases"]),
              "rows", d["meta"]["rows"])
print()
print("### host stamps + validity")
starts, ends = timeline_windows()
samples = monitor_samples()
for r in REPS + ["u"]:
    for c in CELLS:
        t = f"s1u-{c}" if r == "u" else f"{r}s1b-{c}"
        tm, la, gw, pr = precheck(t)
        nn, b, h, mi, det = validity(t, samples, ts(tm), ts(ends[t][0]))
        print(f"| `{t}` | {tm} | {la} | {gw} s | {pr} | {nn} | {b} | {h} | {mi} |")
