"""Headline matrix: B - A at p50 per shape, cores and rows. usage: gen_summary.py"""
import glob, json, random, statistics

B = "/home/cdkbs/bench-runs/bh-close"
ARMS = [("insert", "INSERT, pk omitted"), ("insert-again", "INSERT again (replicate of the line above)"), ("select-pk", "SELECT by pk"),
        ("update-pk", "UPDATE by pk"), ("delete-pk", "DELETE by pk"), ("range100", "COUNT/MIN/MAX over 100 ids (scans the relation)"),
        ("ping", "SHOW META (control)")]
random.seed(1)


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


data = {}
for d, c in (("overhead-c1", 1), ("overhead-c2", 2)):
    random.seed(1)  # same draw order as gen_tables.py, so a cell's CI is the same number in both
    for r in (200, 1000, 10000):
        summ = [json.load(open(f)) for f in sorted(glob.glob(f"{B}/{d}/n{r}-r*.json"))]
        for a, _ in ARMS:
            d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
            dm = [s["arms"]["B"][a]["mean_us"] - s["arms"]["A"][a]["mean_us"] for s in summ]
            dq = [(s["arms"]["B"][a]["qps"] / s["arms"]["A"][a]["qps"] - 1) * 100 for s in summ]
            pa = statistics.median(s["arms"]["A"][a]["p50_us"] for s in summ)
            c50 = boot(d50, statistics.median); boot(dm, statistics.mean); boot(dq, statistics.median)
            data[(a, c, r)] = (statistics.median(d50), c50, statistics.median(dq), pa)
cells = [(None, c, r) for c in (1, 2) for r in (200, 1000, 10000)]
hdr = "| shape | " + " | ".join(f"cores {c}, {r:,} rows" for _, c, r in cells) + " |"
print(hdr)
print("|---|" + "---|" * len(cells))
for a, desc in ARMS:
    row = []
    for _, c, r in cells:
        m, ci, q, pa = data[(a, c, r)]
        row.append(f"{m:+.2f} µs [{ci[0]:+.2f}, {ci[1]:+.2f}], {q:+.1f} % qps")
    print(f"| {desc} | " + " | ".join(row) + " |")
print("\nA p50 per cell, µs:")
for a, desc in ARMS:
    print(f"| {desc} | " + " | ".join(f"{data[(a, c, r)][3]:.1f} µs" for _, c, r in cells) + " |")
