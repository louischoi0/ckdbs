import sys, math
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

SIZES = [200, 1000, 10000]
REPS = ["", "r2-", "r3-"]
SHAPES = ["loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre", "loans-by-daterange", "overdue",
          "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]
# relation size that the shape walks: loans (n), reservations (n/2), books (n/5), users (n/5) at --matches 5
ROWS = {"loans-by-user": 1, "loans-by-book": 1, "resv-by-user": 0.5, "books-by-author": 0.2, "books-by-genre": 0.2, "loans-by-daterange": 1,
        "overdue": 1, "join-loan-user": 1, "join-no-literal": 1, "exists-correlated": 1, "count-by-user": 1}


def fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    return my - b * mx, b


print("### pk-user mean µs per run (the control: no row count, no index)")
print("| cores | n | mode | run 1 | run 2 | run 3 | max/min-1 |")
print("|---|---|---|---|---|---|---|")
for c in (1, 8):
    for n in SIZES:
        for m in ("none", "all"):
            v = [ph(load(f"{r}s3-c{c}-n{n}-{m}"), "pk-user")["mean_us"] for r in REPS]
            print(f"| {c} | {n:,} | {m} | " + " | ".join(f"{x:.1f} µs" for x in v) + f" | {100*(max(v)/min(v)-1):.1f} % |")
print()
allpk = [ph(load(f"{r}s3-c{c}-n{n}-{m}"), "pk-user")["mean_us"] for r in REPS for c in (1, 8) for n in SIZES for m in ("none", "all")]
print("pk-user mean over 36 cells: min %.1f median %.1f max %.1f" % (min(allpk), statistics.median(allpk), max(allpk)))
print("cells above 85 µs:", sum(x > 85 for x in allpk))
print()
print("### per-row fit, unindexed, cores 1: mean = a + b * rows, least squares over n = 200, 1000, 10000 (medians of 3), now / 9a0525d")
print("| shape | rows walked per n | fixed a now | slope b now | fixed a 9a0525d | slope b 9a0525d | b ratio then/now |")
print("|---|---|---|---|---|---|---|")
for s in SHAPES:
    xs = [n * ROWS[s] for n in SIZES]
    y1 = [statistics.median([ph(load(f"{r}s3-c1-n{n}-none"), s)["mean_us"] for r in REPS]) for n in SIZES]
    y0 = [statistics.median([ph(load(f"{r}s3-c1-n{n}-none", 3), s)["mean_us"] for r in REPS]) for n in SIZES]
    a1, b1 = fit(xs, y1)
    a0, b0 = fit(xs, y0)
    print(f"| {s} | {ROWS[s]} x n | {a1:.1f} µs | {1000*b1:.0f} ns per row | {a0:.1f} µs | {1000*b0:.0f} ns per row | {b0/b1:.2f}x |")
print()
PGF = "/tmp/claude-1000/-home-cdkbs-ckdbs/ff2cbc3f-cf1f-478c-a618-6507b57727ba/scratchpad/old/bench/v3.0.0/results-scenario3-library-pg18-v2.7.0-545-gf2f1ee7.md"
txt = open(PGF).read()
i = txt.index("derived qps (1,000,000 / mean")
pg = {}
for l in txt[i:].split("\n"):
    if l.startswith("|"):
        c = [x.strip() for x in l.strip("|").split("|")]
        if c[0].startswith("---") or c[0] == "shape":
            continue
        pg[c[0]] = [float(x.replace(" qps", "").replace(",", "")) for x in c[1:]]
    elif pg:
        break
print("### PG18 (reused) per-row slope from its qps medians (none columns: idx 0,2,4)")
for s in SHAPES:
    xs = [n * ROWS[s] for n in SIZES]
    ys = [1e6 / pg[s][k] for k in (0, 2, 4)]
    a, b = fit(xs, ys)
    print(f"| {s} | a {a:.1f} µs | b {1000*b:.0f} ns per row |")
print()
print("### load phase and create-index per run (cores 1): load mean µs, p50")
for n in SIZES:
    for m in ("none", "all"):
        v = [ph(load(f"{r}s3-c1-n{n}-{m}"), "load") for r in REPS]
        ci = [ph(load(f"{r}s3-c1-n{n}-{m}"), "create-index") for r in REPS]
        print(n, m, [f"{x['mean_us']:.0f}/{x['p50_us']:.0f}" for x in v], [f"{x['mean_us']:.0f}" if x else "-" for x in ci])
print()
print("### cores 8 vs cores 1, same-shape medians: geometric mean of c8/c1 qps per column")
for n in SIZES:
    for m in ("none", "all"):
        r = []
        for s in ["pk-user"] + SHAPES:
            r.append(statistics.median([qps_from_mean(ph(load(f"{rr}s3-c8-n{n}-{m}"), s)) for rr in REPS]) /
                     statistics.median([qps_from_mean(ph(load(f"{rr}s3-c1-n{n}-{m}"), s)) for rr in REPS]))
        print(n, m, "%.3f" % math.exp(sum(math.log(x) for x in r) / len(r)))
