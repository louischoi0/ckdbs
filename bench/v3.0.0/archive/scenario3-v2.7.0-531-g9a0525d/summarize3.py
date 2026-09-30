#!/usr/bin/env python3
import json, statistics
J = "/home/cdkbs/bench-runs/rebaseline-9a0525d/json"
L = "/home/cdkbs/bench-runs/rebaseline-9a0525d/logs"
SIZES = [200, 1000, 10000]
MODES = ["none", "all"]
REPS = ["", "r2-", "r3-"]
SHAPES = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre",
          "loans-by-daterange", "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]

def load(tag):
    return json.load(open(f"{J}/{tag}.json"))

def ph(d, n):
    for p in d["phases"]:
        if p["phase"] == n:
            return p

def qps(p):
    return 1e6 / p["mean_us"]

print("### QPS matrix, cores = 1, median of three runs (derived: 1,000,000 / mean us, serial single connection)")
print("| shape | " + " | ".join(f"n={n:,} {m}" for n in SIZES for m in MODES) + " |")
print("|---|" + "---|" * 6)
for s in SHAPES:
    row = []
    for n in SIZES:
        for m in MODES:
            v = [qps(ph(load(f"{r}s3-c1-n{n}-{m}"), s)) for r in REPS]
            row.append(f"{statistics.median(v):,.0f} qps")
    print(f"| {s} | " + " | ".join(row) + " |")
print()
print("### QPS matrix, cores = 8, median of three runs (derived)")
print("| shape | " + " | ".join(f"n={n:,} {m}" for n in SIZES for m in MODES) + " |")
print("|---|" + "---|" * 6)
for s in SHAPES:
    row = []
    for n in SIZES:
        for m in MODES:
            v = [qps(ph(load(f"{r}s3-c8-n{n}-{m}"), s)) for r in REPS]
            row.append(f"{statistics.median(v):,.0f} qps")
    print(f"| {s} | " + " | ".join(row) + " |")
print()
print("### noise floor: run-to-run spread (max/min - 1) of the per-cell mean, over three runs; median and worst over shapes")
print("| cores | n | mode | median spread over shapes | worst shape | worst spread |")
print("|---|---|---|---|---|---|")
for c in (1, 8):
    for n in SIZES:
        for m in MODES:
            sp = []
            for s in SHAPES:
                v = [ph(load(f"{r}s3-c{c}-n{n}-{m}"), s)["mean_us"] for r in REPS]
                sp.append((max(v) / min(v) - 1, s))
            med = statistics.median([x for x, _ in sp])
            w = max(sp)
            print(f"| {c} | {n:,} | {m} | {100*med:.1f} % | {w[1]} | {100*w[0]:.1f} % |")
print()
print("### the index's effect: none over all, mean-latency ratio (cores = 1, median of three runs); >1 means the index is faster")
print("| shape | n=200 | n=1,000 | n=10,000 |")
print("|---|---|---|---|")
for s in SHAPES:
    row = []
    for n in SIZES:
        a = statistics.median([ph(load(f"{r}s3-c1-n{n}-none"), s)["mean_us"] for r in REPS])
        b = statistics.median([ph(load(f"{r}s3-c1-n{n}-all"), s)["mean_us"] for r in REPS])
        row.append(f"{a/b:.2f}x")
    print(f"| {s} | " + " | ".join(row) + " |")
print()
print("### one-shot phases (cores = 1, run 1; not matrix rows, in their own units)")
print("| n | mode | load ops | load p50 | load mean | create-index ops | create-index p50 | create-index mean |")
print("|---|---|---|---|---|---|---|---|")
for n in SIZES:
    for m in MODES:
        d = load(f"s3-c1-n{n}-{m}")
        l = ph(d, "load"); ci = ph(d, "create-index")
        cis = (f"{ci['ops']} | {ci['p50_us']:,.0f} µs | {ci['mean_us']:,.0f} µs") if ci else "not run | - | -"
        print(f"| {n:,} | {m} | {l['ops']:,} | {l['p50_us']:,.0f} µs | {l['mean_us']:,.0f} µs | {cis} |")
print()
for c in (1,):
    print(f"### percentiles, cores = {c}, run 1, µs (200 ops per shape)")
    print("| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan |")
    print("|---|---|---|---|---|---|---|---|---|")
    for n in SIZES:
        for m in MODES:
            d = load(f"s3-c{c}-n{n}-{m}")
            plans = d["meta"]["plans"]
            for s in SHAPES:
                p = ph(d, s)
                pl = ",".join(plans.get(s, plans.get(s.replace("resv", "resv"), ["-"])))
                print(f"| {n:,} | {m} | {s} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {pl} |")
print()
print("### host stamps")
print("| cell | precheck UTC | loadavg | build procs | driver exit | verify problems |")
print("|---|---|---|---|---|---|")
for r in REPS:
    for c in (1, 8):
        for n in SIZES:
            for m in MODES:
                tag = f"{r}s3-c{c}-n{n}-{m}"
                t = open(f"{L}/{tag}.precheck").read().split("\n")
                tm = t[0].split(": ")[1]; la = " / ".join(t[1].split(": ")[1].split()[:3])
                bp = "none" if "  none" in "\n".join(t) else "SEE"
                rc = open(f"{L}/{tag}.rc").read().strip()
                vp = len(load(tag)["meta"]["verify_problems"])
                print(f"| `{tag}` | {tm} | {la} | {bp} | {rc} | {vp} |")
