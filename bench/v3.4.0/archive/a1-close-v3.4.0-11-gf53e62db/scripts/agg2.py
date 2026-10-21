import json, glob, statistics, re
O = "/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-fix-ddl-build-fault-net/2fe39e3f-40fc-42ed-910b-fde09fa4cb64/scratchpad/out/"
K = ("p0", "p25", "p50", "p90", "p99", "max")
def f(x): return f"{x:,.1f}"
print("### stmt percentile tables (median across 10 reps of each percentile, us)")
print("| rows | statement | build | n/rep | p0 | p25 | p50 | p90 | p99 | max | QPS (1e6/mean) | p50 rep range |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for size in ("200", "1000", "10000"):
    for ph in ("insert", "point-select", "update", "full-scan"):
        for lab in "AB":
            ds = [json.load(open(p))[size][ph] for p in sorted(glob.glob(O + f"stmt-{lab}-*.json"))]
            med = lambda k: statistics.median(d[k] for d in ds)
            p50s = [d["p50"] for d in ds]
            slow = sum(1 for x in p50s if x > 55 * (1 if ph != "full-scan" else 1e9) and ph != "full-scan" and x > 1.25 * min(p50s))
            q = 1e6 / med("mean")
            nrep = ds[0]["n"]
            print(f"| {size} | {ph} | {lab} | {nrep} | " + " | ".join(f(med(k)) for k in K) +
                  f" | {q:,.0f} | {f(min(p50s))}-{f(max(p50s))} (slow-mode reps {slow}) |")
print()
print("### deltas B-A of median p50 and best-rep p50")
for size in ("200", "1000", "10000"):
    for ph in ("insert", "point-select", "update", "full-scan"):
        r = {}
        for lab in "AB":
            ds = [json.load(open(p))[size][ph] for p in sorted(glob.glob(O + f"stmt-{lab}-*.json"))]
            r[lab] = ([d["p50"] for d in ds], [d["mean"] for d in ds])
        mA, mB = statistics.median(r["A"][0]), statistics.median(r["B"][0])
        bA, bB = min(r["A"][0]), min(r["B"][0])
        qa, qb = 1e6 / statistics.median(r["A"][1]), 1e6 / statistics.median(r["B"][1])
        print(f"{size} {ph}: median-p50 A {mA:.1f} B {mB:.1f} ({(mB-mA)/mA*100:+.1f}%), best-rep p50 A {bA:.1f} B {bB:.1f} ({(bB-bA)/bA*100:+.1f}%), QPS(median mean) A {qa:,.0f} B {qb:,.0f} ({(qb-qa)/qa*100:+.1f}%), A-spread {min(r['A'][0]):.1f}-{max(r['A'][0]):.1f}")
print()
print("### build")
for lab in "AB":
    walls = {"CREATE INDEX": [], "CREATE ASSERTION": []}
    for p in sorted(glob.glob(O + f"build-{lab}-*.json")):
        d = json.load(open(p))
        for k in walls:
            walls[k].append(d[k]["build_wall_s"])
    print(lab, walls, {k: round(statistics.mean(v), 3) for k, v in walls.items()})
print("B probe distributions per rep")
for lab in "AB":
    for k in ("CREATE INDEX", "CREATE ASSERTION"):
        ds = [json.load(open(p))[k] for p in sorted(glob.glob(O + f"build-{lab}-*.json"))]
        for d in ds:
            pd = d["probes_during"]
            print(lab, k, d["build_wall_s"], pd["n"], d["completed_inside"], [pd[x] for x in K], d["slow_probes"])
print("idle", [(json.load(open(p))["idle"]["n"], json.load(open(p))["idle"]["p50"], json.load(open(p))["idle"]["p99"], json.load(open(p))["idle"]["max"]) for p in sorted(glob.glob(O + "build-*.json"))])
la = []; busy = 0
for line in open(O + "env.log"):
    m = re.search(r"loadavg=(\S+)", line)
    la.append(float(m.group(1)))
    if "procs=[]" not in line: busy += 1
print("env.log lines", len(la), "load1 min/max", min(la), max(la), "lines with cc1plus/cmake/ctest", busy)
