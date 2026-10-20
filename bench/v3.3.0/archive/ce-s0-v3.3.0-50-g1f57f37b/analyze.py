#!/usr/bin/env python3
"""CE-S0 analysis: reads the run directory, prints the markdown tables of the results file.
usage: analyze.py [RUN_DIR]   (default: the run directory this file is in)"""
import json, os, statistics, sys, glob

RUN = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
SHAPES = ["range-1", "range-100", "range-1000", "range-10000", "range-60480", "filter-sym3"]
ROWS = {"range-1": 1, "range-100": 100, "range-1000": 1000, "range-10000": 10000, "range-60480": 60480, "filter-sym3": 7560}
MODES = ["send", "split", "drain"]


def pct(xs, p):
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p / 100.0
    lo = int(k); hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def f(x, d=1):
    return f"{x:,.{d}f}"


def us(x, d=1):
    return f"{x:,.{d}f} µs"


def fit(xs, ys):
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sl = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return sl, my - sl * mx


cells = [json.loads(l) for l in open(f"{RUN}/rowcost/cells.jsonl")]
by = {}
for c in cells:
    by.setdefault((c["shape"], c["mode"]), []).append(c)


def med(shape, mode, key):
    return statistics.median(c[key] for c in by[(shape, mode)])


def idle(c):
    return c["wall_us_per_stmt"] - c["client_cpu_us_per_stmt"]


def medidle(shape, mode):
    return statistics.median(idle(c) for c in by[(shape, mode)])


def spread(shape, mode, key="wall_us_per_stmt"):
    xs = [c[key] for c in by[(shape, mode)]]
    m = statistics.median(xs)
    return 100 * (max(xs) - min(xs)) / m


print("### per-statement components, median of 3 runs (µs per statement)\n")
print("| shape | rows returned | client path | stmts per run | wall | spread | client CPU | idle (wall - client CPU) | idle / wall | server CPU | idle / server CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for s in SHAPES:
    for m in MODES:
        if (s, m) not in by:
            continue
        w, cc, sc = med(s, m, "wall_us_per_stmt"), med(s, m, "client_cpu_us_per_stmt"), med(s, m, "server_cpu_us_per_stmt")
        i = medidle(s, m)
        n = statistics.median(c["n"] for c in by[(s, m)])
        print(f"| {s} | {ROWS[s]:,} rows | {m} | {n:.0f} | {us(w)} | {spread(s, m):.1f} % | {us(cc)} | {us(i)} | {100 * i / w:.1f} % | {us(sc)} | {i / sc:.2f} |")

print("\n### per returned row (µs per row), range shapes: marginal = (cost(size) - cost(1 row)) / (size - 1); slope = least-squares over the sizes named\n")
print("| client path | quantity | marginal 100 | marginal 1,000 | marginal 10,000 | marginal 60,480 | slope, sizes 1..10,000 | slope, sizes 1..60,480 | intercept (fit 1..60,480) |")
print("|---|---|---|---|---|---|---|---|---|")
rs = ["range-1", "range-100", "range-1000", "range-10000", "range-60480"]
for m in MODES:
    for qn, getter in (("wall", lambda s, m=m: med(s, m, "wall_us_per_stmt")),
                       ("client CPU", lambda s, m=m: med(s, m, "client_cpu_us_per_stmt")),
                       ("idle", lambda s, m=m: medidle(s, m)),
                       ("server CPU", lambda s, m=m: med(s, m, "server_cpu_us_per_stmt"))):
        base = getter("range-1")
        marg = [(getter(s) - base) / (ROWS[s] - 1) for s in rs[1:]]
        x4 = [ROWS[s] for s in rs[:4]]; y4 = [getter(s) for s in rs[:4]]
        x5 = [ROWS[s] for s in rs]; y5 = [getter(s) for s in rs]
        sl4, _ = fit(x4, y4); sl5, ic5 = fit(x5, y5)
        print(f"| {m} | {qn} | " + " | ".join(f"{v:.3f} µs/row" for v in marg) + f" | {sl4:.3f} µs/row | {sl5:.3f} µs/row | {ic5:,.1f} µs |")

print("\n### the filtered walk: WHERE symbol_id = 3 (60,480 rows scanned, 7,560 returned)\n")
print("| client path | wall | client CPU | idle | idle / wall | server CPU | idle / server CPU | server CPU per returned row | server CPU per scanned row | idle per returned row |")
print("|---|---|---|---|---|---|---|---|---|---|")
for m in MODES:
    s = "filter-sym3"
    w, cc, sc, i = med(s, m, "wall_us_per_stmt"), med(s, m, "client_cpu_us_per_stmt"), med(s, m, "server_cpu_us_per_stmt"), medidle(s, m)
    print(f"| {m} | {us(w)} | {us(cc)} | {us(i)} | {100 * i / w:.1f} % | {us(sc)} | {i / sc:.2f} | {sc / 7560:.3f} µs/row | {sc / 60480:.3f} µs/row | {i / 7560:.3f} µs/row |")

print("\n### per-statement latency distribution, `send` path (the scenario drivers' own), all statements of the 3 runs pooled\n")
print("| shape | rows returned | statements | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|")
for s in SHAPES:
    for m in ("send", "drain"):
        if (s, m) not in by:
            continue
        xs = [x for c in by[(s, m)] for x in c["lat_us"]]
        print(f"| {s} ({m}) | {ROWS[s]:,} rows | {len(xs):,} | " + " | ".join(us(pct(xs, p)) for p in (0, 25, 50, 95, 99)) + " |")

# time to first batch
tf = json.load(open(f"{RUN}/json/ttfb.json"))
print("\n### time to first byte, first S_ROW_BATCH and end of reply (µs from send; client pinned CPU 6, server CPU 2)\n")
print("| shape | rows returned | reps | batches | bytes | event | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for label, d in tf.items():
    reps = d["reps"]
    r0 = reps[len(reps) // 2]
    for i, e in enumerate(("first_byte", "row_desc", "first_batch", "last_batch", "ready")):
        xs = [r[e] for r in reps]
        head = f"| {label} | {r0['rows']:,} rows | {len(reps)} | {r0['batches']} | {r0['bytes']:,} B " if i == 0 else "| | | | | "
        print(f"{head}| {e} | " + " | ".join(us(pct(xs, p)) for p in (0, 25, 50, 95, 99)) + " |")
print("\n### first batch against end of reply (medians)\n")
print("| shape | rows returned | first byte | first batch | reply complete (ready) | first batch / ready | ready - first batch | recv calls |")
print("|---|---|---|---|---|---|---|---|")
for label, d in tf.items():
    reps = d["reps"]
    m = lambda e: statistics.median(r[e] for r in reps)
    print(f"| {label} | {reps[0]['rows']:,} rows | {us(m('first_byte'))} | {us(m('first_batch'))} | {us(m('ready'))} | {100 * m('first_batch') / m('ready'):.1f} % | {us(m('ready') - m('first_batch'))} | {statistics.median(r['recvs'] for r in reps):.0f} |")

# scenario cells
def load(tag):
    p = f"{RUN}/json/{tag}.json"
    return json.load(open(p)) if os.path.exists(p) else None


print("\n### scenario 1, cores = 1, group (driver-reported)\n")
tags = ["s1-c1-g", "r2-s1-c1-g", "r3-s1-c1-g"]
ds = [load(t) for t in tags]
ds = [d for d in ds if d]
print("matrix (warm = same 8 arguments cycled, 100 statements; cold = each argument first seen):\n")
print("| shape | rows returned | warm QPS run 1 | run 2 | run 3 | median | spread | cold QPS median | warm mean (median of runs) | warm p50 (median of runs) |")
print("|---|---|---|---|---|---|---|---|---|---|")
SR = {"bar-range": 200, "symbol-history": 7560, "model-join": 359, "day-slice": 8}
for sh in ("bar-range", "symbol-history", "model-join", "day-slice"):
    ws, cs, wm, wp = [], [], [], []
    for d in ds:
        e = [x for x in d["meta"]["qps_sweep"] if x["shape"] == sh][0]
        ws.append(e["warm"]["qps"]); cs.append(e["cold"]["qps"]); wm.append(e["warm"]["mean_us"]); wp.append(e["warm"]["p50_us"])
    m = statistics.median(ws)
    print(f"| {sh} | {SR[sh]:,} rows | " + " | ".join(f"{v:,.1f} qps" for v in ws) + f" | {m:,.1f} qps | {100 * (max(ws) - min(ws)) / m:.1f} % | {statistics.median(cs):,.1f} qps | {us(statistics.median(wm))} | {us(statistics.median(wp))} |")
print("\nphase distributions (the driver's own phases; p0 p25 p50 p95 p99 over each run's statements, run 1 / 2 / 3 medians of each percentile):\n")
print("| phase | rows returned | runs | ops per run | p0 | p25 | p50 | p95 | p99 | QPS from mean (median of runs) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for ph, nm in (("read-bar-range", 200), ("read-symbol-history", 7560), ("read-day-slice", 8)):
    ps = [[p for p in d["phases"] if p["phase"] == ph][0] for d in ds]
    print(f"| {ph} | {nm:,} rows | {len(ps)} | {ps[0]['ops']} | " + " | ".join(us(statistics.median(p[k] for p in ps)) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")) + f" | {statistics.median(1e6 / p['mean_us'] for p in ps):,.1f} qps |")

print("\n### scenario 3, 10,000 loans, unindexed, cores = 1, group (driver-reported)\n")
tags = ["s3-c1-n10000-none", "r2-s3-c1-n10000-none", "r3-s3-c1-n10000-none"]
ds = [load(t) for t in tags]
ds = [d for d in ds if d]
print("| shape | runs | ops per run | QPS run 1 (derived 1e6 / mean) | run 2 | run 3 | median | spread | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for ph in ("books-by-genre", "loans-by-daterange", "overdue", "loans-by-user"):
    ps = [[p for p in d["phases"] if p["phase"] == ph][0] for d in ds]
    q = [1e6 / p["mean_us"] for p in ps]
    m = statistics.median(q)
    print(f"| {ph} | {len(ps)} | {ps[0]['ops']} | " + " | ".join(f"{v:,.0f} qps" for v in q) + f" | {m:,.0f} qps | {100 * (max(q) - min(q)) / m:.1f} % | " + " | ".join(us(statistics.median(p[k] for p in ps)) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")) + " |")
