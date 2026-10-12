import glob, json
R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05"
moved = 0; refused = 0; n = 0; cores_seen = {}
for f in glob.glob(f"{R}/out/m0/*/n*-r*.json"):
    if f.endswith(".raw.json.gz"):
        continue
    d = json.load(open(f)); n += 1
    for l in "AB":
        for arm, v in d["arms"][l].items():
            moved += v["cpu"]["blocks_moved"]
            for c in v["cpu"].get("cores_seen", []):
                cores_seen[c] = cores_seen.get(c, 0) + 1
        refused = max(refused, d["pool_end"][l].get("pool_refused", 0))
print("runs", n, "blocks moved", moved, "max pool_refused", refused, "cores seen", cores_seen)
a = json.load(open(f"{R}/out/m0/analysis.json"))
for k in ("group-c1|insert|200", "group-c1|insert|1000", "group-c1|insert|10000", "group-c4|insert|200", "relaxed-c1|insert|200"):
    print(k, "A cpu", round(a[k]["Ac"], 2), "A p50", a[k]["A50"], "A qps", round(a[k]["Aq"]))
