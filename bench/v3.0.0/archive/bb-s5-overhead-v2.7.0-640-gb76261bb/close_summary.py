import glob, json, random, re, statistics as st, sys

D = sys.argv[1]
groups = {}
for f in sorted(glob.glob(D + "/conc/*.json")):
    tag = re.sub(r"-r\d+\.json$", "", f.split("/")[-1])
    groups.setdefault(tag, []).append(json.load(open(f)))
print("cell runs rows/s p0 p25 p50 p95 p99 refusals both_cores_runs")
for tag, runs in sorted(groups.items()):
    m = lambda k: st.median(r[k] for r in runs)
    spread = sum(1 for r in runs if len(set(r["client_cores"])) > 1)
    print(tag, len(runs), round(m("rows_per_s")), *[round(m(k), 1) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")],
          m("refusals"), f"{spread}/{len(runs)}")


def boot(d):
    rs = random.Random(7)
    meds = [st.median(rs.choice(d) for _ in d) for _ in range(4000)]
    meds.sort()
    return meds[100], meds[3899]


# paired p50 deltas per run, by shape: B run r minus A run r
for cell in ("c3-k2", "c4-k2", "c3-k8", "c4-k8"):
    A = {r["run"]: r for r in groups.get(cell + "-A", [])}
    B = {r["run"]: r for r in groups.get(cell + "-B", [])}
    for k in ("p50_us", "rows_per_s"):
        d = [B[i][k] - A[i][k] for i in sorted(A) if i in B]
        lo, hi = boot(d)
        print(cell, k, "dB-A median", round(st.median(d), 1), "CI95", round(lo, 1), round(hi, 1), "n", len(d))

c5 = [json.load(open(f)) for f in sorted(glob.glob(D + "/c5/*.json"))]
print("c5 runs", len(c5), "same", all(r["same_reply_AB"]["limit"] and r["same_reply_AB"]["full"] for r in c5))
for arm in ("limit", "limit-again", "full", "ping"):
    for lab in ("A", "B"):
        print("c5", arm, lab, *[round(st.median(r["arms"][lab][arm][k] for r in c5), 1) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")])
    d = [r["arms"]["B"][arm]["p50_us"] - r["arms"]["A"][arm]["p50_us"] for r in c5]
    lo, hi = boot(d)
    a = st.median(r["arms"]["A"][arm]["p50_us"] for r in c5)
    print("c5", arm, "dB-A p50", round(st.median(d), 1), "CI95", round(lo, 1), round(hi, 1), f"{100*st.median(d)/a:+.2f}%")
print("c5 probe", c5[0].get("probe_B_descending_reply"))
