import glob, json, random, statistics, sys

D = sys.argv[1]
random.seed(11)
arms = ["lit-point", "lit-point-again", "lit2-point", "lit-range", "insert", "select-pk", "delete-pk", "ping"]
pairs = [("s0-s1", "080cd555 (S0)", "586ffa5f (+ BJ-Q12)"), ("s0-s2", "080cd555 (S0)", "6a4fb896 (+ Q12, S1, S2)"),
         ("s0-s3", "080cd555 (S0)", "88155d5b (+ Q12, S1..S3)"), ("s0-A", "080cd555 (S0)", "e5a7e1b7 (A, + BA)"),
         ("A-s4", "e5a7e1b7 (A)", "836b9551 (BA + S0..S3)"), ("A-s5", "e5a7e1b7 (A)", "1a251cea (+ S4)"),
         ("A-s6", "e5a7e1b7 (A)", "3292b20d (+ S5, S6)")]


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


print("| pair (left vs right) | runs | " + " | ".join(arms) + " |")
print("|---|---|" + "---|" * len(arms))
for tag, l, r in pairs:
    fs = sorted(glob.glob(f"{D}/{tag}-c1-n1000-r*.json"))
    js = [json.load(open(f)) for f in fs]
    cells = []
    for a in arms:
        d = [j["arms"]["B"][a]["p50_us"] - j["arms"]["A"][a]["p50_us"] for j in js]
        c = boot(d, statistics.median)
        m = statistics.median(d)
        if "range" in a:
            rel = statistics.median([(j["arms"]["B"][a]["p50_us"] / j["arms"]["A"][a]["p50_us"] - 1) * 100 for j in js])
            cells.append(f"{m:+.0f} µs [{c[0]:+.0f}, {c[1]:+.0f}] ({rel:+.1f} %)")
        else:
            cells.append(f"{m:+.2f} µs [{c[0]:+.2f}, {c[1]:+.2f}]")
    print(f"| {l} vs {r} | {len(js)} | " + " | ".join(cells) + " |")
