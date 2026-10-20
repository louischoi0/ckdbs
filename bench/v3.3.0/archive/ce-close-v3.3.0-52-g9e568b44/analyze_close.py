#!/usr/bin/env python3
"""CE close analysis: A (base) against B (CE), three interleaved pairs. usage: analyze_close.py [RUN_DIR]
Spread = (max - min) / median over the 3 runs of one binary. A B/A difference is a finding only when it exceeds the larger of the two spreads."""
import json, os, statistics, sys

RUN = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
PAIRS = [1, 2, 3]
SHAPES = ["range-1", "range-100", "range-1000", "range-10000", "range-60480", "filter-sym3"]
ROWS = {"range-1": 1, "range-100": 100, "range-1000": 1000, "range-10000": 10000, "range-60480": 60480, "filter-sym3": 7560}
SLICES = {"range-1": 1, "range-100": 1, "range-1000": 1, "range-10000": 2.28, "range-60480": 13.70, "filter-sym3": 13.70}
MODES = ["send", "split"]
med = statistics.median


def pct(xs, p):
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p / 100.0
    lo = int(k); hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def spr(xs):
    m = med(xs)
    return 100 * (max(xs) - min(xs)) / m if m else 0.0


def us(x, d=1):
    return f"{x:,.{d}f} µs"


def verdict(a, b):
    """a, b: lists of per-run values (higher or lower, the sign is the reader's). returns (ratio %, spreadA, spreadB, finding?)"""
    ma, mb = med(a), med(b)
    d = 100 * (mb - ma) / ma
    sa, sb = spr(a), spr(b)
    return d, sa, sb, abs(d) > max(sa, sb)


def vtxt(a, b):
    d, sa, sb, f = verdict(a, b)
    return f"{d:+.1f} %", f"{sa:.1f} % / {sb:.1f} %", "**yes**" if f else "no (inside the spread)"


# ---------- sweep ----------
cells = {}
for b in "AB":
    for p in PAIRS:
        fn = f"{RUN}/rowcost/cells-p{p}-{b}.jsonl"
        if not os.path.exists(fn):
            continue
        for l in open(fn):
            c = json.loads(l)
            cells.setdefault((b, c["shape"], c["mode"]), []).append(c)


def vals(b, s, m, key):
    out = []
    for c in cells.get((b, s, m), []):
        out.append(key(c))
    return out


wall = lambda c: c["wall_us_per_stmt"]
ccpu = lambda c: c["client_cpu_us_per_stmt"]
scpu = lambda c: c["server_cpu_us_per_stmt"]
idle = lambda c: c["wall_us_per_stmt"] - c["client_cpu_us_per_stmt"]

print("## Row-cost sweep, per statement, median of the runs (A = base, B = CE)\n")
print("Runs per binary:", {b: len(cells.get((b, 'range-1', 'send'), [])) for b in 'AB'}, "\n")
for m in MODES:
    print(f"### `{m}` path\n")
    print("| shape | rows returned | quantity | A | B | B over A | spread A / B | a finding? |")
    print("|---|---|---|---|---|---|---|---|")
    for s in SHAPES:
        for qn, key in (("wall", wall), ("client CPU", ccpu), ("idle (wall - client CPU)", idle), ("server CPU", scpu)):
            a, b = vals("A", s, m, key), vals("B", s, m, key)
            if not a or not b:
                continue
            r, sp, f = vtxt(a, b)
            print(f"| {s} | {ROWS[s]:,} rows | {qn} | {us(med(a))} | {us(med(b))} | {r} | {sp} | {f} |")
    print()

print("## Hidden fraction against CE-S0's ceiling\n")
print("Ceiling = A's idle x (1 - 1 / slices) (CE-S0 section 6), as a share of A's wall. Measured = (A wall - B wall) / A wall, medians. Idle removed = A idle - B idle.\n")
print("| path | shape | rows returned | slices (CE-S0 fractional) | A wall | B wall | measured hidden | measured, share of A wall | ceiling, share of A wall | wall hidden / ceiling | A idle | B idle | idle removed | idle removed / ceiling | spread of wall A / B |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for m in MODES:
    for s in SHAPES:
        a, b = vals("A", s, m, wall), vals("B", s, m, wall)
        if not a or not b:
            continue
        ia, ib = med(vals("A", s, m, idle)), med(vals("B", s, m, idle))
        wa, wb = med(a), med(b)
        ceil = ia * (1 - 1 / SLICES[s])
        hid = wa - wb
        print(f"| {m} | {s} | {ROWS[s]:,} rows | {SLICES[s]} | {us(wa)} | {us(wb)} | {us(hid)} | {100 * hid / wa:+.1f} % | {100 * ceil / wa:.1f} % | "
              + (f"{hid / ceil:.2f}" if ceil > 0 else "n/a (no boundary)") + f" | {us(ia)} | {us(ib)} | {us(ia - ib)} | " + (f"{(ia - ib) / ceil:.2f}" if ceil > 0 else "n/a") + f" | {spr(a):.1f} % / {spr(b):.1f} % |")
print()

print("## Per-statement latency distribution (all statements of the runs pooled)\n")
print("| path | shape | rows returned | binary | statements | p0 | p25 | p50 | p95 | p99 |")
print("|---|---|---|---|---|---|---|---|---|---|")
for m in MODES:
    for s in SHAPES:
        for b in "AB":
            xs = [x for c in cells.get((b, s, m), []) for x in c["lat_us"]]
            if xs:
                print(f"| {m} | {s} | {ROWS[s]:,} rows | {b} | {len(xs):,} | " + " | ".join(us(pct(xs, p)) for p in (0, 25, 50, 95, 99)) + " |")
print()

# ---------- ttfb ----------
tf = {}
for b in "AB":
    for p in PAIRS:
        fn = f"{RUN}/json/ttfb-p{p}-{b}.json"
        if os.path.exists(fn):
            tf.setdefault(b, []).append(json.load(open(fn)))
if tf:
    print("## Time to first byte, first S_ROW_BATCH and end of reply (µs from send)\n")
    print("Each run's percentile over its own reps (100, fewer at 60,480 rows), then the median over the runs of that binary. Runs: " + str({b: len(v) for b, v in tf.items()}) + "\n")
    print("| shape | rows returned | batches (A / B) | event | binary | p0 | p25 | p50 | p95 | p99 | spread of p50 over runs |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    labels = list(tf["A"][0].keys())
    for label in labels:
        for e in ("first_byte", "first_batch", "ready"):
            for b in "AB":
                runs = [r[label]["reps"] for r in tf[b]]
                ps = [[pct([x[e] for x in reps], p) for p in (0, 25, 50, 95, 99)] for reps in runs]
                bat = {bb: med(x["batches"] for x in tf[bb][0][label]["reps"]) for bb in "AB"}
                rows = tf[b][0][label]["reps"][0]["rows"]
                p50s = [q[2] for q in ps]
                head = f"| {label} | {rows:,} rows | {bat['A']:.0f} / {bat['B']:.0f} " if (e == "first_byte" and b == "A") else "| | | "
                print(f"{head}| {e} | {b} | " + " | ".join(us(med(q[i] for q in ps)) for i in range(5)) + f" | {spr(p50s):.1f} % |")
    print("\n### First-batch time and the share of the reply it has reached (medians of the runs' p50)\n")
    print("| shape | rows returned | A first batch | B first batch | B over A | spread A / B | a finding? | A first batch / A ready | B first batch / B ready | A ready | B ready | ready B over A |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for label in labels:
        p50 = lambda b, e: [med(x[e] for x in r[label]["reps"]) for r in tf[b]]
        fa, fb, ra, rb = p50("A", "first_batch"), p50("B", "first_batch"), p50("A", "ready"), p50("B", "ready")
        r, sp, f = vtxt(fa, fb)
        rows = tf["A"][0][label]["reps"][0]["rows"]
        print(f"| {label} | {rows:,} rows | {us(med(fa))} | {us(med(fb))} | {r} | {sp} | {f} | {100 * med(fa) / med(ra):.1f} % | {100 * med(fb) / med(rb):.1f} % | {us(med(ra))} | {us(med(rb))} | {vtxt(ra, rb)[0]} |")
    print("\n### Reply shape check (first frame types, batches, bytes, errors), A and B\n")
    print("| shape | binary | first_types of rep 0 | batches | bytes | rows | error flags in any rep |")
    print("|---|---|---|---|---|---|---|")
    for label in labels:
        for b in "AB":
            reps = tf[b][0][label]["reps"]
            err = sum(x["error"] for r in tf[b] for x in r[label]["reps"])
            print(f"| {label} | {b} | {reps[0]['first_types']} | {reps[0]['batches']} | {reps[0]['bytes']:,} B | {reps[0]['rows']:,} | {err} |")
    print()


# ---------- scenarios ----------
def load(tag):
    p = f"{RUN}/json/{tag}.json"
    return json.load(open(p)) if os.path.exists(p) else None


s1 = {b: [d for d in (load(f"p{p}-{b}-s1") for p in PAIRS) if d] for b in "AB"}
if s1["A"] and s1["B"]:
    print("## Scenario 1 (60,480 bars), c1-g, warm QPS (driver matrix)\n")
    print("| shape | rows returned | A, runs | B, runs | A median | B median | B over A | spread A / B | a finding? |")
    print("|---|---|---|---|---|---|---|---|---|")
    SR = {"bar-range": 200, "symbol-history": 7560, "model-join": 359, "day-slice": 8}
    for sh in SR:
        q = {}
        for b in "AB":
            q[b] = [[x for x in d["meta"]["qps_sweep"] if x["shape"] == sh][0]["warm"]["qps"] for d in s1[b]]
        r, sp, f = vtxt(q["A"], q["B"])
        print(f"| {sh} | {SR[sh]:,} rows | " + " / ".join(f"{v:,.1f}" for v in q["A"]) + " qps | " + " / ".join(f"{v:,.1f}" for v in q["B"]) + f" qps | {med(q['A']):,.1f} qps | {med(q['B']):,.1f} qps | {r} | {sp} | {f} |")
    print("\nCold QPS (each argument first seen):\n")
    print("| shape | A median | B median | B over A | spread A / B | a finding? |")
    print("|---|---|---|---|---|---|")
    for sh in SR:
        q = {b: [[x for x in d["meta"]["qps_sweep"] if x["shape"] == sh][0]["cold"]["qps"] for d in s1[b]] for b in "AB"}
        r, sp, f = vtxt(q["A"], q["B"])
        print(f"| {sh} | {med(q['A']):,.1f} qps | {med(q['B']):,.1f} qps | {r} | {sp} | {f} |")
    print("\nPhase distributions (each run's percentile, median over the runs):\n")
    print("| phase | rows returned | binary | ops per run | p0 | p25 | p50 | p95 | p99 |")
    print("|---|---|---|---|---|---|---|---|---|")
    for ph, nm in (("read-bar-range", 200), ("read-symbol-history", 7560), ("read-day-slice", 8)):
        for b in "AB":
            ps = [[p for p in d["phases"] if p["phase"] == ph][0] for d in s1[b]]
            print(f"| {ph} | {nm:,} rows | {b} | {ps[0]['ops']} | " + " | ".join(us(med(p[k] for p in ps)) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")) + " |")
    print()

s3 = {b: [d for d in (load(f"p{p}-{b}-s3") for p in PAIRS) if d] for b in "AB"}
if s3["A"] and s3["B"]:
    print("## Scenario 3 (10,000 loans, unindexed), c1-g, QPS derived as 1,000,000 / mean µs\n")
    print("| shape | A, runs | B, runs | A median | B median | B over A | spread A / B | a finding? |")
    print("|---|---|---|---|---|---|---|---|")
    PH = ("books-by-genre", "loans-by-daterange", "overdue", "loans-by-user")
    for ph in PH:
        q = {b: [1e6 / [p for p in d["phases"] if p["phase"] == ph][0]["mean_us"] for d in s3[b]] for b in "AB"}
        r, sp, f = vtxt(q["A"], q["B"])
        print(f"| {ph} | " + " / ".join(f"{v:,.0f}" for v in q["A"]) + " qps | " + " / ".join(f"{v:,.0f}" for v in q["B"]) + f" qps | {med(q['A']):,.0f} qps | {med(q['B']):,.0f} qps | {r} | {sp} | {f} |")
    print("\nPhase distributions (each run's percentile, median over the runs):\n")
    print("| phase | binary | ops per run | p0 | p25 | p50 | p95 | p99 |")
    print("|---|---|---|---|---|---|---|---|")
    for ph in PH:
        for b in "AB":
            ps = [[p for p in d["phases"] if p["phase"] == ph][0] for d in s3[b]]
            print(f"| {ph} | {b} | {ps[0]['ops']} | " + " | ".join(us(med(p[k] for p in ps)) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")) + " |")
    print()
