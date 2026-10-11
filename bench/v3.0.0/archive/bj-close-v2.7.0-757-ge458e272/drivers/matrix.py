"""Headline matrices from the run JSONs: throughput A vs B per shape per (cores, rows) cell, and the p50 delta.
usage: matrix.py <dir> [--max-other-busy X]"""
import glob, json, random, statistics, sys

D = sys.argv[1]
THR = float(sys.argv[sys.argv.index("--max-other-busy") + 1]) if "--max-other-busy" in sys.argv else 0.30
random.seed(5)
SH = [("lit-point", "`UPDATE SET v = <lit> WHERE id = k`"), ("lit-point-again", "same shape, second relation (replicate)"),
      ("lit2-point", "`UPDATE SET v = <lit>, w = <lit> WHERE id = k`"),
      ("lit-range", "`UPDATE SET v = <lit> WHERE id >= 1` (every row)"), ("lit-range-again", "same shape, second relation (replicate)"),
      ("insert", "`INSERT`, pk named (not touched by BJ)"), ("select-pk", "`SELECT * WHERE id = k` (control)"),
      ("delete-pk", "`DELETE WHERE id = k` (not touched by BJ)"), ("ping", "`SHOW META` (control)")]


def boot(v, fn, n=4000):
    b = sorted(fn(random.choices(v, k=len(v))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


cells = {}
for f in sorted(glob.glob(f"{D}/c*-n*-r*.json")):
    j = json.load(open(f))
    sc = {2, 4} | ({3} if j["cores"] == 2 else set())
    o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
    if sum(o) / len(o) > THR or not all(j["verify"].values()) or j["bad"]:
        continue
    cells.setdefault((j["cores"], j["rows"]), []).append(j)
keys = sorted(cells)

print("P50-DERIVED THROUGHPUT (1,000,000 / p50)")
print("| shape | " + " | ".join(f"cores {c}, {r:,} rows" for c, r in keys) + " |")
print("|---|" + "---|" * len(keys))
for a, nm in SH:
    row = []
    for k in keys:
        js = cells[k]
        pa = statistics.median(j["arms"]["A"][a]["p50_us"] for j in js)
        pb = statistics.median(j["arms"]["B"][a]["p50_us"] for j in js)
        d = [(j["arms"]["A"][a]["p50_us"] / j["arms"]["B"][a]["p50_us"] - 1) * 100 for j in js]
        c = boot(d, statistics.median)
        row.append(f"{1e6 / pa:,.0f} → {1e6 / pb:,.0f} /s ({statistics.median(d):+.1f} % [{c[0]:+.1f}, {c[1]:+.1f}])")
    print(f"| {nm} | " + " | ".join(row) + " |")
print()
print("DRIVER THROUGHPUT (ops / elapsed)")
print("| shape | " + " | ".join(f"cores {c}, {r:,} rows" for c, r in keys) + " |")
print("|---|" + "---|" * len(keys))
for a, nm in SH:
    row = []
    for k in keys:
        js = cells[k]
        qa = statistics.median(j["arms"]["A"][a]["qps"] for j in js)
        qb = statistics.median(j["arms"]["B"][a]["qps"] for j in js)
        d = [(j["arms"]["B"][a]["qps"] / j["arms"]["A"][a]["qps"] - 1) * 100 for j in js]
        c = boot(d, statistics.median)
        row.append(f"{qa:,.0f} → {qb:,.0f} /s ({statistics.median(d):+.1f} % [{c[0]:+.1f}, {c[1]:+.1f}])")
    print(f"| {nm} | " + " | ".join(row) + " |")
print()
print("| shape | " + " | ".join(f"cores {c}, {r:,} rows" for c, r in keys) + " |")
print("|---|" + "---|" * len(keys))
for a, nm in SH:
    row = []
    for k in keys:
        js = cells[k]
        pa = statistics.median(j["arms"]["A"][a]["p50_us"] for j in js)
        d = [j["arms"]["B"][a]["p50_us"] - j["arms"]["A"][a]["p50_us"] for j in js]
        c = boot(d, statistics.median)
        m = statistics.median(d)
        if "range" in a:
            row.append(f"{pa:,.0f} µs: {m:+.1f} µs [{c[0]:+.1f}, {c[1]:+.1f}] ({m / pa * 100:+.1f} %)")
        else:
            row.append(f"{pa:.1f} µs: {m:+.2f} µs [{c[0]:+.2f}, {c[1]:+.2f}] ({m / pa * 100:+.1f} %)")
    print(f"| {nm} | " + " | ".join(row) + " |")
