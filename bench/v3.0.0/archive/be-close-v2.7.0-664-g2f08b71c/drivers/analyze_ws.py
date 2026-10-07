import glob, gzip, json, random, statistics, sys
D = "/home/cdkbs/bench-runs/be-close/warmscan"
arms = ["scan-all", "scan-all-again"]


def pct(s, p):
    k = max(0, min(len(s) - 1, int(-(-p * len(s) // 100)) - 1))
    return s[k]


random.seed(1)
for rows in (10000, 100000):
    files = sorted(glob.glob(f"{D}/n{rows}-r*.raw.json.gz"))
    summ = [json.load(open(f.replace(".raw.json.gz", ".json"))) for f in files]
    raws = [json.load(gzip.open(f, "rt")) for f in files]
    print(f"## rows={rows} runs={len(files)} loads={[s['host_before']['loadavg'].split()[0] for s in summ]} errs={sum(a['errors'] for s in summ for l in s['arms'].values() for a in l.values())}")
    print("counts", {json.dumps(s['count_be_a']) for s in summ})
    print("rss_hwm_kb A/B median", statistics.median(s['rss_hwm_kb']['A'] for s in summ), statistics.median(s['rss_hwm_kb']['B'] for s in summ))
    for a in arms:
        row = {}
        for L in "AB":
            allv = sorted(x for r in raws for x in r[L][a])
            row[L] = [min(allv)] + [pct(allv, p) for p in (25, 50, 90, 99)] + [len(allv), sum(allv) / len(allv)]
        # per-run p50 delta and mean delta, bootstrap
        d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
        dm = [s["arms"]["B"][a]["mean_us"] - s["arms"]["A"][a]["mean_us"] for s in summ]
        bs = sorted(statistics.median(random.choices(d50, k=len(d50))) for _ in range(4000))
        bm = sorted(statistics.mean(random.choices(dm, k=len(dm))) for _ in range(4000))
        qa = statistics.median(s["arms"]["A"][a]["qps"] for s in summ)
        qb = statistics.median(s["arms"]["B"][a]["qps"] for s in summ)
        print(f" {a:13s} A p0/25/50/90/99 = {row['A'][0]:.1f}/{row['A'][1]:.1f}/{row['A'][2]:.1f}/{row['A'][3]:.1f}/{row['A'][4]:.1f} mean={row['A'][6]:.2f} | "
              f"B = {row['B'][0]:.1f}/{row['B'][1]:.1f}/{row['B'][2]:.1f}/{row['B'][3]:.1f}/{row['B'][4]:.1f} mean={row['B'][6]:.2f} n={row['A'][5]} | "
              f"dp50 med={statistics.median(d50):+.2f} CI[{bs[100]:+.2f},{bs[3900]:+.2f}] dmean={statistics.mean(dm):+.2f} CI[{bm[100]:+.2f},{bm[3900]:+.2f}] qps A={qa:.0f} B={qb:.0f}")
