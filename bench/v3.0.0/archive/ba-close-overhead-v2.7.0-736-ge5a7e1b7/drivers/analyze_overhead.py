import glob, gzip, json, random, statistics, sys

D = sys.argv[1]
arms = ["insert", "insert-again", "select-pk", "update-pk", "delete-pk", "bulk20", "range100", "ping"]


def pct(s, p):
    k = max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))
    return s[k]


random.seed(1)


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


import os
for rows in [r for r in (200, 1000, 10000) if glob.glob(f"{D}/n{r}-r*.raw.json.gz")]:
    files = sorted(glob.glob(f"{D}/n{rows}-r*.raw.json.gz"))
    summ = [json.load(open(f.replace(".raw.json.gz", ".json"))) for f in files]
    raws = [json.load(gzip.open(f, "rt")) for f in files]
    loads = [float(s["host_before"]["loadavg"].split()[0]) for s in summ]
    comp = set()
    for s in summ:
        comp |= set(s["host_before"]["competing"]) | set(s["host_after"]["competing"])
    ck = [s["mount_ckpt_us"] for s in summ]
    print(f"## rows={rows} runs={len(files)} load1 before: min {min(loads)} max {max(loads)} median {statistics.median(loads)}; "
          f"competing={sorted(comp)} errs={sum(a['errors'] for s in summ for l in s['arms'].values() for a in l.values())}")
    print("counts", {json.dumps(s['count_be_a']) for s in summ},
          "ckpt_us median A/B", statistics.median(c["A"] for c in ck), statistics.median(c["B"] for c in ck),
          "max", max(c["A"] for c in ck), max(c["B"] for c in ck))
    for a in arms:
        row = {}
        for L in "AB":
            allv = sorted(x for r in raws for x in r[L][a])
            row[L] = dict(n=len(allv), p0=allv[0], p25=pct(allv, 25), p50=pct(allv, 50), p75=pct(allv, 75),
                          p95=pct(allv, 95), p99=pct(allv, 99), mean=sum(allv) / len(allv))
            rm = [s["arms"][L][a]["mean_us"] for s in summ]
            row[L]["mean_sd"] = statistics.stdev(rm)
            row[L]["qps_med"] = statistics.median(s["arms"][L][a]["qps"] for s in summ)
        d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
        dm = [s["arms"]["B"][a]["mean_us"] - s["arms"]["A"][a]["mean_us"] for s in summ]
        dq = [(s["arms"]["B"][a]["qps"] / s["arms"]["A"][a]["qps"] - 1) * 100 for s in summ]
        c50 = boot(d50, statistics.median); cm = boot(dm, statistics.mean); cq = boot(dq, statistics.median)
        print(f"{a}|{row['A']['n']}|"
              f"A {row['A']['p0']:.1f}/{row['A']['p25']:.1f}/{row['A']['p50']:.1f}/{row['A']['p75']:.1f}/{row['A']['p95']:.1f}/{row['A']['p99']:.1f} mean {row['A']['mean']:.2f}±{row['A']['mean_sd']:.2f} qps {row['A']['qps_med']:.0f}|"
              f"B {row['B']['p0']:.1f}/{row['B']['p25']:.1f}/{row['B']['p50']:.1f}/{row['B']['p75']:.1f}/{row['B']['p95']:.1f}/{row['B']['p99']:.1f} mean {row['B']['mean']:.2f}±{row['B']['mean_sd']:.2f} qps {row['B']['qps_med']:.0f}|"
              f"dp50 {statistics.median(d50):+.2f} [{c50[0]:+.2f},{c50[1]:+.2f}]|dmean {statistics.mean(dm):+.2f} [{cm[0]:+.2f},{cm[1]:+.2f}]|dqps% {statistics.median(dq):+.2f} [{cq[0]:+.2f},{cq[1]:+.2f}]")
    # within-arm replicate: same binary, same shape, two relations (insert vs insert-again)
    for L in "AB":
        dd = [s["arms"][L]["insert-again"]["p50_us"] - s["arms"][L]["insert"]["p50_us"] for s in summ]
        print(f" replicate {L}: insert-again - insert p50 median {statistics.median(dd):+.2f} us, sd {statistics.stdev(dd):.2f}")
