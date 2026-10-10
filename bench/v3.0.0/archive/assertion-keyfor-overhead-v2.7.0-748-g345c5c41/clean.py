import glob, json, re, statistics as st
OUT = "/home/cdkbs/bench-runs/keyfor"
ARMS = ["insert-assert-existing", "insert-assert-newgroup", "update-assert", "delete-assert",
        "insert-none-existing", "insert-none-again", "update-none", "update-none-again", "delete-none", "ping"]
bad = set()
for h in glob.glob(f"{OUT}/*-run*/host.txt"):
    t = open(h).read()
    load = float(re.search(r"loadavg_at_start=([\d.]+)", t).group(1))
    if load > 1.2 or re.search(r"^\d+ (ctest|cmake |\S*cc1plus)", t, re.M):
        bad.add(h.split("/")[-2])
print("excluded", sorted(bad))
for g in ("int", "date"):
    for n in (200, 1000, 10000):
        runs = []
        for f in sorted(glob.glob(f"{OUT}/{g}-n{n}-run*/result.json")):
            if f.split("/")[-2] in bad:
                continue
            runs.append(json.load(open(f)))
        out = []
        for a in ARMS:
            d = [r["arms"]["B"][a]["p50_us"] - r["arms"]["A"][a]["p50_us"] for r in runs]
            out.append(f"{a}={st.median(d):+.2f}")
        print(g, n, len(runs), " ".join(out))
