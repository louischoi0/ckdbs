import json, statistics as st
d = json.load(open("/home/cdkbs/bvclose/run/final/out.json"))
R = len(d["runs"])
cell = {}
for r in d["runs"]:
    for c in r["cells"]:
        cell[(c["rows"], c["shape"], c["path"], c["bin"], r["run"])] = c
keys = sorted({k[:3] for k in cell}, key=lambda k: (k[1] != "update", k[2] != "literal", k[0]))
arms = ["A1", "A2", "B", "BT"]
print("### T1 cpu")
print("| Shape | Path | Rows | A1 | A2 | B | BT | A2-A1 (floor) | B-A | BT-B |\n|---|---|---|---|---|---|---|---|---|---|")
for k in keys:
    m = {a: st.mean(cell[k + (a, r)]["server_cpu_us"] for r in range(R)) for a in arms}
    f = [cell[k + ("A2", r)]["server_cpu_us"] - cell[k + ("A1", r)]["server_cpu_us"] for r in range(R)]
    ab = [cell[k + ("B", r)]["server_cpu_us"] - (cell[k + ("A1", r)]["server_cpu_us"] + cell[k + ("A2", r)]["server_cpu_us"]) / 2 for r in range(R)]
    bt = [cell[k + ("BT", r)]["server_cpu_us"] - cell[k + ("B", r)]["server_cpu_us"] for r in range(R)]
    s = lambda x: "%+.2f µs (sd %.2f)" % (st.mean(x), st.pstdev(x))
    print("| %s | %s | %d | " % (k[1].upper(), k[2], k[0]) + " | ".join("%.2f µs" % m[a] for a in arms) + " | %s | %s | %s |" % (s(f), s(ab), s(bt)))
print("### T2 qps")
print("| Shape | Path | Rows | A1 | A2 | B | BT |\n|---|---|---|---|---|---|---|")
for k in keys:
    print("| %s | %s | %d | " % (k[1].upper(), k[2], k[0]) + " | ".join("{:,.0f} stmt/s".format(1e6 / st.mean(cell[k + (a, r)]["mean_us"] for r in range(R))) for a in arms) + " |")
print("### T3 lat")
print("| Shape | Path | Rows | Arm | ops | p0 | p25 | p50 | p95 | p99 |\n|---|---|---|---|---|---|---|---|---|---|")
for k in keys:
    for a in arms:
        l = [st.median(cell[k + (a, r)][p] for r in range(R)) for p in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us")]
        print("| %s | %s | %d | %s | %s | " % (k[1].upper(), k[2], k[0], a, "{:,}".format(sum(cell[k + (a, r)]["ops"] for r in range(R)))) + " | ".join("%.1f µs" % x for x in l) + " |")
