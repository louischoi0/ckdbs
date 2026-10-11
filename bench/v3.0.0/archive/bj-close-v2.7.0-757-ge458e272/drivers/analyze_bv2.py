import glob, json, random, statistics, sys
D = sys.argv[1]
random.seed(13)
arms = ["lit-point", "lit-point-again", "lit2-point", "lit-range", "insert", "select-pk", "delete-pk", "ping"]


def boot(v, fn, n=4000):
    b = sorted(fn(random.choices(v, k=len(v))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


js = [json.load(open(f)) for f in sorted(glob.glob(f"{D}/BvB2-c1-n1000-r*.json"))]
print(f"runs {len(js)}; left = B 8e37baf4, right = B' e458e272; median per-run p50 delta right - left, 95 % CI")
print("| shape | left p50 | right - left | relative |")
print("|---|---|---|---|")
for a in arms:
    d = [j["arms"]["B"][a]["p50_us"] - j["arms"]["A"][a]["p50_us"] for j in js]
    r = [(j["arms"]["B"][a]["p50_us"] / j["arms"]["A"][a]["p50_us"] - 1) * 100 for j in js]
    pl = statistics.median(j["arms"]["A"][a]["p50_us"] for j in js)
    c = boot(d, statistics.median)
    cr = boot(r, statistics.median)
    print(f"| {a} | {pl:.1f} µs | {statistics.median(d):+.2f} µs [{c[0]:+.2f}, {c[1]:+.2f}] | {statistics.median(r):+.2f} % [{cr[0]:+.2f}, {cr[1]:+.2f}] |")
