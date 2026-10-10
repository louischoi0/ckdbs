"""Part 2 absolute p50 of B' (b2/out-c1, c1 1,000 rows) against 8e37baf4 (b2/out-flip), two independent sets of 16 runs.
Difference of the medians of per-run p50, bootstrap 95 % CI over both sets."""
import glob, json, random, statistics
random.seed(21)
R = "/home/cdkbs/bench-runs/bj-close/b2/"
b2 = [json.load(open(f)) for f in sorted(glob.glob(R + "out-all/c1-n1000-r*.json"))]
fl = [json.load(open(f)) for f in sorted(glob.glob(R + "out-flip/c1-n1000-r*.json"))]
print("| B-only arm | 8e37baf4 p50 (flipped runs) | B' p50 | B' - 8e37baf4 |")
print("|---|---|---|---|")
for k, rows in (("xp-lit", 1), ("xp-expr", 1), ("xp-swap", 1), ("xp-case", 1), ("xr-lit", 1000), ("xr-expr", 1000), ("xr-swap", 1000), ("xr-case", 1000), ("xr-subq", 1000), ("rt-one", 1), ("rt-pair", 1)):
    a = [j["arms_b_only"][k]["p50_us"] for j in fl]
    b = [j["arms_b_only"][k]["p50_us"] for j in b2]
    d = statistics.median(b) - statistics.median(a)
    bs = sorted(statistics.median(random.choices(b, k=len(b))) - statistics.median(random.choices(a, k=len(a))) for _ in range(4000))
    lo, hi = bs[100], bs[3900]
    if rows == 1:
        print(f"| {k} | {statistics.median(a):.1f} µs | {statistics.median(b):.1f} µs | {d:+.2f} µs [{lo:+.2f}, {hi:+.2f}] |")
    else:
        print(f"| {k} (per row) | {statistics.median(a) / rows * 1000:.0f} ns | {statistics.median(b) / rows * 1000:.0f} ns | {d / rows * 1000:+.0f} ns [{lo / rows * 1000:+.0f}, {hi / rows * 1000:+.0f}] |")
