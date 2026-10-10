"""Headline matrices. usage: ba_matrix.py <label:dir> ... ; each dir is one (cores) configuration. Cells (rows 200/1000/10000)."""
import glob, json, random, statistics, sys

DESC = {"insert": "INSERT, pk omitted", "insert-again": "INSERT again (replicate)", "select-pk": "SELECT by pk",
        "update-pk": "UPDATE by pk", "delete-pk": "DELETE by pk", "bulk20": "bulk INSERT, 20 rows",
        "range100": "whole-relation scan (COUNT/MIN/MAX, 100-id window)", "ping": "SHOW META", "ping2": "SHOW NAMESPACES (control)", "iso": "SET ISOLATION LEVEL (control)"}
random.seed(1)


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


specs = [a.split(":", 1) for a in sys.argv[1:]]
data = {}
arms_seen = []
for label, d in specs:
    for r in (200, 1000, 10000):
        summ = [json.load(open(f)) for f in sorted(glob.glob(f"{d}/n{r}-r*.json"))]
        if not summ:
            continue
        for a in DESC:
            if a not in summ[0]["arms"]["A"]:
                continue
            if a not in arms_seen:
                arms_seen.append(a)
            d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
            dq = [(s["arms"]["B"][a]["qps"] / s["arms"]["A"][a]["qps"] - 1) * 100 for s in summ]
            data[(a, label, r)] = (statistics.median(d50), boot(d50, statistics.median), statistics.median(dq), boot(dq, statistics.median),
                                   statistics.median(s["arms"]["A"][a]["qps"] for s in summ), statistics.median(s["arms"]["B"][a]["qps"] for s in summ),
                                   statistics.median(s["arms"]["A"][a]["p50_us"] for s in summ), len(summ))
cols = [(l, r) for l, _ in specs for r in (200, 1000, 10000)]
print("| shape | " + " | ".join(f"{l}, {r:,} rows" for l, r in cols) + " |")
print("|---|" + "---|" * len(cols))
for a in [x for x in DESC if x in arms_seen]:
    row = []
    for l, r in cols:
        v = data.get((a, l, r))
        row.append("-" if not v else f"{v[0]:+.2f} µs [{v[1][0]:+.2f}, {v[1][1]:+.2f}]; {v[2]:+.1f} % tps [{v[3][0]:+.1f}, {v[3][1]:+.1f}]")
    print(f"| {DESC[a]} | " + " | ".join(row) + " |")
print("\nThroughput, A then B (median of runs' ops/elapsed):\n")
print("| shape | " + " | ".join(f"{l}, {r:,} rows" for l, r in cols) + " |")
print("|---|" + "---|" * len(cols))
for a in [x for x in DESC if x in arms_seen]:
    row = []
    for l, r in cols:
        v = data.get((a, l, r))
        row.append("-" if not v else f"{v[4]:,.0f} -> {v[5]:,.0f} tps")
    print(f"| {DESC[a]} | " + " | ".join(row) + " |")
print("\nA's p50:\n")
print("| shape | " + " | ".join(f"{l}, {r:,} rows" for l, r in cols) + " |")
print("|---|" + "---|" * len(cols))
for a in [x for x in DESC if x in arms_seen]:
    print(f"| {DESC[a]} | " + " | ".join("-" if not data.get((a, l, r)) else f"{data[(a, l, r)][6]:.1f} µs" for l, r in cols) + " |")
