import glob, json, statistics as st
OUT = "/home/cdkbs/bench-runs/keyfor"
arms = ["insert-assert-existing", "insert-assert-newgroup", "update-assert", "delete-assert",
        "insert-none-existing", "update-none", "delete-none", "ping"]
for g in ("int", "date"):
    for n in (200, 1000, 10000):
        runs = [json.load(open(f)) for f in sorted(glob.glob(f"{OUT}/{g}-n{n}-run*/result.json"))]
        for a in arms:
            row = []
            for l in "AB":
                row.append(" ".join("%6.1f" % st.median(r["arms"][l][a][k] for r in runs)
                                    for k in ("p0_us", "p25_us", "p50_us", "p75_us", "p95_us", "p99_us")))
            print(f"{g:4} {n:5} {a:<24} A[{row[0]}] B[{row[1]}] runs={len(runs)} ops={runs[0]['ops_per_arm']}")
