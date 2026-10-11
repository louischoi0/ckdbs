#!/usr/bin/env python3
"""gen_tables.py RES OUTDIR -> markdown table fragments (one file per fragment)."""
import json, os, sys

RES, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
c1 = json.load(open(f"{RES}/c1-analysis.json"))
ARMS = [("select", "pk point SELECT"), ("insert", "single-row INSERT"), ("insert-again", "single-row INSERT, repeated (replicate)"),
        ("update", "one-row UPDATE by pk"), ("ping", "SHOW NAMESPACES (client/socket floor)")]
SIZES = [200, 1000, 10000]


def f1(x, s=False):
    return f"{x:+.2f}" if s else f"{x:,.1f}"


def qps(us):
    return 1e6 / us


# ---- cell 1 matrix
rows = ["| Statement | Rows | A QPS | B QPS | B-A QPS | B-A % | d p50 us, main med [IQR] | B>A pairs | d p50 us, swap med | binary effect x us | slot effect s us |",
        "|---|---|---|---|---|---|---|---|---|---|---|"]
for arm, label in ARMS:
    for n in SIZES:
        c = c1[f"{n}|{arm}"]["p50"]
        a, b = qps(c["A"]), qps(c["B"])
        dm, ds = c["d_main"], c["d_swap"]
        rows.append(f"| {label} | {n:,} | {a:,.0f} | {b:,.0f} | {b - a:+,.0f} | {100 * (b - a) / a:+.2f} % | "
                    f"{dm['med']:+.2f} [{dm['q1']:+.2f}, {dm['q3']:+.2f}] | {dm['B_gt_A']}/{c['pairs_main']} | "
                    f"{ds['med']:+.2f} ({ds['B_gt_A']}/{c['pairs_swap']}) | {c['x']:+.2f} | {c['s']:+.2f} |")
open(f"{OUT}/c1_matrix.md", "w").write("\n".join(rows) + "\n")

# ---- cell 1 distributions
rows = ["| Statement | Rows | Engine | p0 | p25 | p50 | p75 | p99 | engine us/stmt |", "|---|---|---|---|---|---|---|---|---|"]
for arm, label in ARMS:
    for n in SIZES:
        cell = c1[f"{n}|{arm}"]
        for eng in ("A", "B"):
            rows.append(f"| {label if eng == 'A' else ''} | {n:,} | {eng} | " + " | ".join(
                f"{cell[k][eng]:,.1f}" for k in ("p0", "p25", "p50", "p75", "p99")) + f" | {cell['eng_us'][eng]:.2f} |")
open(f"{OUT}/c1_dist.md", "w").write("\n".join(rows) + "\n")

# ---- cell 1 engine-time and p99/mean deltas
rows = ["| Statement | Rows | d engine us/stmt, main med [IQR] | d p0 | d p25 | d p75 | d p99 | d mean |", "|---|---|---|---|---|---|---|---|"]
for arm, label in ARMS:
    for n in SIZES:
        cell = c1[f"{n}|{arm}"]
        e = cell["eng_us"]["d_main"]
        rows.append(f"| {label} | {n:,} | {e['med']:+.2f} [{e['q1']:+.2f}, {e['q3']:+.2f}] | " + " | ".join(
            f"{cell[k]['d_main']['med']:+.2f}" for k in ("p0", "p25", "p75", "p99", "mean")) + " |")
open(f"{OUT}/c1_eng.md", "w").write("\n".join(rows) + "\n")

# ---- cells 2/3 from agg.json
def agg(cell):
    return json.load(open(f"{RES}/{cell}/agg.json"))


def row(a, key, lab, unit="us"):
    r = a[key]
    return (f"| {lab} | {r['A']:,.1f} | {r['B']:,.1f} | {r['A2']:,.1f} | {r['B-A']['med']:+,.1f} [{r['B-A']['q1']:+,.1f}, {r['B-A']['q3']:+,.1f}] | "
            f"{r['B-A']['pos']}/{r['triplets']} | {r['A2-A']['med']:+,.1f} [{r['A2-A']['q1']:+,.1f}, {r['A2-A']['q3']:+,.1f}] |")


HDR = ["| Measure | A | B | A2 (A again) | B-A med [IQR] | B>A triplets | A2-A med [IQR] (floor) |", "|---|---|---|---|---|---|---|"]


def block(cell, specs):
    a = agg(cell)
    out = list(HDR)
    for key, lab in specs:
        out.append(row(a, key, lab))
    return out


LAT = ["p0", "p25", "p50", "p75", "p99", "max"]
for cell in ("c2", "c3b"):
    for n in SIZES:
        d = f"{cell}-n{n}"
        if not os.path.exists(f"{RES}/{d}/agg.json"):
            continue
        spec = [(f"reader|select|{s}", f"read {s} us") for s in LAT] + [("reader|select|qps", "read QPS")]
        spec += [("writer|unit|qps", "writer commits/s" + (" (relaxed INSERT)" if cell == "c2" else " (strict)"))]
        spec += [("writer|unit|p50", "writer unit p50 us"), ("writer|unit|p99", "writer unit p99 us")]
        open(f"{OUT}/{d}.md", "w").write("\n".join(block(d, spec)) + "\n")
for n in SIZES:
    d = f"c3-n{n}"
    if not os.path.exists(f"{RES}/{d}/agg.json"):
        continue
    spec = [(f"reader|select|{s}", f"mix SELECT {s} us") for s in LAT] + [(f"reader|insert|{s}", f"mix INSERT {s} us") for s in LAT]
    spec += [("reader|select|qps", "mix statements/s (SELECT + INSERT)"), ("writer|unit|qps", "strict commits/s"),
             ("writer|unit|p50", "strict unit p50 us"), ("writer|unit|p99", "strict unit p99 us"),
             ("ceiling|contention_ceiling_waits|v", "ceiling waits per run (instance)"),
             ("ceiling|contention_ceiling_wait_us|v", "ceiling wait us per run (instance)")]
    open(f"{OUT}/{d}.md", "w").write("\n".join(block(d, spec)) + "\n")
for n in SIZES:
    d = f"c3c-n{n}"
    if not os.path.exists(f"{RES}/{d}/agg.json"):
        continue
    spec = [(f"bystander|select|{s}", f"bystander SELECT {s} us") for s in LAT] + [("bystander|select|qps", "bystander QPS"),
                                                                                  ("bystander|select|n", "bystander SELECTs per run")]
    spec += [(f"reader|select|{s}", f"mix SELECT {s} us") for s in LAT] + [(f"reader|insert|{s}", f"mix INSERT {s} us") for s in LAT]
    spec += [("reader|select|qps", "mix statements/s (SELECT + INSERT)"), ("writer|unit|qps", "strict commits/s"),
             ("writer|unit|p50", "strict unit p50 us"), ("writer|unit|p99", "strict unit p99 us"),
             ("ceiling|contention_ceiling_waits|v", "ceiling waits per run (instance)"),
             ("ceiling|contention_ceiling_wait_us|v", "ceiling wait us per run (instance)")]
    open(f"{OUT}/{d}.md", "w").write("\n".join(block(d, spec)) + "\n")
print(sorted(os.listdir(OUT)))

# ---- size sweep for cells 2/3/3c
SW = [("c2", "read p50 us", "reader|select|p50"), ("c2", "read p99 us", "reader|select|p99"),
      ("c2", "read QPS", "reader|select|qps"),
      ("c3", "mix statements/s", "reader|select|qps"), ("c3", "strict commits/s", "writer|unit|qps"),
      ("c3c", "bystander SELECT p50 us", "bystander|select|p50"), ("c3c", "bystander SELECT p99 us", "bystander|select|p99"),
      ("c3c", "bystander QPS", "bystander|select|qps"), ("c3c", "mix statements/s", "reader|select|qps"),
      ("c3c", "strict commits/s", "writer|unit|qps")]
rows = ["| Cell | Measure | Rows | A | B | A2 | B-A med [IQR] | B>A | A2-A (floor) | triplets |", "|---|---|---|---|---|---|---|---|---|---|"]
for cell, lab, key in SW:
    for n in SIZES:
        a = json.load(open(f"{RES}/{cell}-n{n}/agg.json"))[key]
        rows.append(f"| {cell} | {lab} | {n:,} | {a['A']:,.1f} | {a['B']:,.1f} | {a['A2']:,.1f} | {a['B-A']['med']:+,.1f} [{a['B-A']['q1']:+,.1f}, {a['B-A']['q3']:+,.1f}] | "
                    f"{a['B-A']['pos']}/{a['triplets']} | {a['A2-A']['med']:+,.1f} | {a['triplets']} |")
open(f"{OUT}/sweep.md", "w").write("\n".join(rows) + "\n")
