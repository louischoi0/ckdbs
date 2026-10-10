"""Tables for the BA close overhead A/B. usage: ba_tables.py <dir> <label> [summary|full]
Arms are discovered from the run JSONs. Bootstrap: 4,000 resamples of the runs, seed 1."""
import glob, gzip, json, random, statistics, sys

D, LABEL = sys.argv[1], sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "full"
DESC = {"insert": "INSERT, pk omitted", "insert-again": "INSERT again (replicate)", "select-pk": "SELECT by pk",
        "update-pk": "UPDATE by pk", "delete-pk": "DELETE by pk", "bulk20": "bulk INSERT, 20 rows ascending",
        "range100": "COUNT/MIN/MAX over 100 ids (walks the relation)", "ping": "SHOW META (control)",
        "ping2": "SHOW NAMESPACES (control)", "iso": "SET ISOLATION LEVEL (control)"}
ORDER = list(DESC)


def pct(s, p):
    k = max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))
    return s[k]


random.seed(1)


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


def f(x): return f"{x:,.1f}"


out = []
for rows in [r for r in (200, 1000, 10000) if glob.glob(f"{D}/n{r}-r*.raw.json.gz")]:
    files = sorted(glob.glob(f"{D}/n{rows}-r*.raw.json.gz"))
    summ = [json.load(open(x.replace(".raw.json.gz", ".json"))) for x in files]
    raws = [json.load(gzip.open(x, "rt")) for x in files]
    arms = [a for a in ORDER if a in summ[0]["arms"]["A"]]
    loads = [float(s["host_before"]["loadavg"].split()[0]) for s in summ]
    comp = [s["host_before"]["competing"] + s["host_after"]["competing"] for s in summ]
    ncomp = sum(1 for c in comp if any(("ctest" in e or "cc1plus" in e or "kds_tests" in e) for e in c))
    errs = sum(a["errors"] for s in summ for l in s["arms"].values() for a in l.values())
    n_per = len(raws[0]["A"][arms[0]]) * len(files)
    out.append(f"\n**{LABEL}, {rows:,} rows** - {len(files)} runs, {n_per:,} statements per arm per shape pooled "
               f"(loadavg before each run {min(loads):.2f}-{max(loads):.2f}; {ncomp} runs with a ctest/cc1plus/kds_tests entry in the competing list; {errs} errors)\n")
    if MODE == "full":
        out.append("| shape | arm | p0 | p25 | p50 | p75 | p95 | p99 | mean (sd of run means) | throughput (median of runs) |")
        out.append("|---|---|---|---|---|---|---|---|---|---|")
    delta = []
    for a in arms:
        for L in "AB":
            allv = sorted(x for r in raws for x in r[L][a])
            rm = [s["arms"][L][a]["mean_us"] for s in summ]
            qps = statistics.median(s["arms"][L][a]["qps"] for s in summ)
            if MODE == "full":
                out.append(f"| {DESC[a]} | {L} | {f(allv[0])} µs | {f(pct(allv,25))} µs | {f(pct(allv,50))} µs | {f(pct(allv,75))} µs | "
                           f"{f(pct(allv,95))} µs | {f(pct(allv,99))} µs | {sum(allv)/len(allv):.1f} µs (sd {statistics.stdev(rm):.1f}) | {qps:,.0f} qps |")
        d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
        dm = [s["arms"]["B"][a]["mean_us"] - s["arms"]["A"][a]["mean_us"] for s in summ]
        dq = [(s["arms"]["B"][a]["qps"] / s["arms"]["A"][a]["qps"] - 1) * 100 for s in summ]
        c50, cm, cq = boot(d50, statistics.median), boot(dm, statistics.mean), boot(dq, statistics.median)
        pa = statistics.median([s["arms"]["A"][a]["p50_us"] for s in summ])
        delta.append((a, statistics.median(d50), c50, statistics.mean(dm), cm, statistics.median(dq), cq, pa))
    out.append(f"\n| shape ({LABEL}, {rows:,} rows) | Δ p50, B - A (95 % CI) | Δ mean (95 % CI) | Δ throughput (95 % CI) | A p50 | reading |")
    out.append("|---|---|---|---|---|---|")
    for a, m50, c50, dmean, cm, mq, cq, pa in delta:
        sig = (c50[0] > 0 or c50[1] < 0)
        if not sig:
            reading = "no difference resolved"
        elif m50 > 0:
            reading = f"**B slower** ({m50 / pa * 100:+.1f} % at p50)"
        else:
            reading = f"B faster ({m50 / pa * 100:+.1f} % at p50)"
        out.append(f"| {DESC[a]} | {m50:+.2f} µs [{c50[0]:+.2f}, {c50[1]:+.2f}] | {dmean:+.2f} µs [{cm[0]:+.2f}, {cm[1]:+.2f}] | "
                   f"{mq:+.2f} % [{cq[0]:+.2f}, {cq[1]:+.2f}] | {pa:.1f} µs | {reading} |")
    if "insert-again" in arms:
        for L in "AB":
            dd = [s["arms"][L]["insert-again"]["p50_us"] - s["arms"][L]["insert"]["p50_us"] for s in summ]
            out.append(f"\nReplicate floor, arm {L}: INSERT again - INSERT p50 per run, median {statistics.median(dd):+.2f} µs, sd {statistics.stdev(dd):.2f} µs.")
print("\n".join(out))
