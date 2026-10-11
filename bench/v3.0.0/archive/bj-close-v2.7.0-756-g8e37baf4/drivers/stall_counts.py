import glob, gzip, json, collections, sys
THR = float(sys.argv[2]) if len(sys.argv) > 2 else 100000.0  # us
for c in (1, 2):
    for rows in (200, 1000, 10000):
        fs = sorted(glob.glob(f"{sys.argv[1]}/out-c{c}/c{c}-n{rows}-r*.raw.json.gz"))
        cnt = collections.Counter(); runs = collections.Counter(); tot = collections.Counter()
        for f in fs:
            r = json.load(gzip.open(f, "rt"))
            for L in "AB":
                hit = set()
                for a, v in r["ab"][L].items():
                    if "range" in a:
                        continue
                    n = sum(1 for x in v if x > THR)
                    if n:
                        cnt[(L, a)] += n; hit.add(a)
                for a in hit:
                    runs[(L, a)] += 1
        print(f"cores={c} rows={rows} runs={len(fs)} (stalls > {THR / 1000:.0f} ms in non-range arms; count over all runs / runs hit)",
              {f"{k[0]}:{k[1]}": f"{cnt[k]}/{runs[k]}" for k in sorted(cnt)})
