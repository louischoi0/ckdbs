import glob, gzip, json, random, statistics
R = "/home/cdkbs/bench-runs/be-close"


def pct(s, p):
    k = max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))
    return s[k]


random.seed(1)


def block(d, sizes, arms, label):
    dist_rows, delta_rows = [], []
    for rows in sizes:
        files = sorted(glob.glob(f"{R}/{d}/n{rows}-r*.raw.json.gz"))
        summ = [json.load(open(f.replace(".raw.json.gz", ".json"))) for f in files]
        raws = [json.load(gzip.open(f, "rt")) for f in files]
        for a in arms:
            st = {}
            for L in "AB":
                allv = sorted(x for r in raws for x in r[L][a])
                mean = sum(allv) / len(allv)
                st[L] = (min(allv), pct(allv, 25), pct(allv, 50), pct(allv, 90), pct(allv, 99), len(allv), mean)
                dist_rows.append(f"| {rows:,} | {a} | {L} | {len(allv):,} | {st[L][0]:,.1f} us | {st[L][1]:,.1f} us | {st[L][2]:,.1f} us | {st[L][3]:,.1f} us | {st[L][4]:,.1f} us |")
            d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
            bs = sorted(statistics.median(random.choices(d50, k=len(d50))) for _ in range(4000))
            qa = [s["arms"]["A"][a]["ops"] / s["arms"]["A"][a]["elapsed_s"] for s in summ]
            qb = [s["arms"]["B"][a]["ops"] / s["arms"]["B"][a]["elapsed_s"] for s in summ]
            qd = [(y - x) / x * 100 for x, y in zip(qa, qb)]
            bq = sorted(statistics.median(random.choices(qd, k=len(qd))) for _ in range(4000))
            ma, mb = st["A"][6], st["B"][6]
            delta_rows.append(
                f"| {rows:,} | {a} | {statistics.median(qa):,.0f} tps | {statistics.median(qb):,.0f} tps | "
                f"{statistics.median(qd):+.2f} % [{bq[100]:+.2f}, {bq[3900]:+.2f}] | {statistics.median(d50):+.2f} us [{bs[100]:+.2f}, {bs[3900]:+.2f}] | "
                f"{(mb - ma):+.2f} us ({(mb - ma) / ma * 100:+.2f} %) |")
    print(f"### {label} - distributions (pooled over {len(files)} runs)\n")
    print("| rows | shape | arm | statements | p0 | p25 | p50 | p90 | p99 |")
    print("|---|---|---|---|---|---|---|---|---|")
    print("\n".join(dist_rows))
    print(f"\n### {label} - throughput delta B vs A (median of per-run values, bootstrap 95 % interval)\n")
    print("| rows | shape | A | B | dB-A throughput | dB-A p50 | dB-A pooled mean |")
    print("|---|---|---|---|---|---|---|")
    print("\n".join(delta_rows))
    print()


block("overhead", (200, 1000, 10000), ["insert", "insert-again", "select-pk", "update-pk", "range100", "ping"], "OLTP shapes")
block("warmscan", (10000, 100000), ["scan-all", "scan-all-again"], "Warm full scan")
