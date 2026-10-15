import sys, math
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

SIZES = [200, 1000, 10000]
MODES = ["none", "all"]
REPS = ["", "r2-", "r3-"]
SHAPES = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre",
          "loans-by-daterange", "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]
PGF = "/tmp/claude-1000/-home-cdkbs-ckdbs/ff2cbc3f-cf1f-478c-a618-6507b57727ba/scratchpad/old/bench/v3.0.0/results-scenario3-library-pg18-v2.7.0-545-gf2f1ee7.md"


def mq(c, n, m, s, old=None):
    return statistics.median([qps_from_mean(ph(load(f"{r}s3-c{c}-n{n}-{m}", old), s)) for r in REPS])


def pgmatrix():
    txt = open(PGF).read()
    i = txt.index("derived qps (1,000,000 / mean")
    out = {}
    for l in txt[i:].split("\n"):
        if not l.startswith("|"):
            if out:
                break
            continue
        c = [x.strip() for x in l.strip("|").split("|")]
        if c[0].startswith("---") or c[0] == "shape":
            continue
        out[c[0]] = [float(x.replace(" qps", "").replace(",", "")) for x in c[1:]]
    return out


PG = pgmatrix()
COLS = [(n, m) for n in SIZES for m in MODES]

for c in (1, 8):
    print(f"### QPS, cores = {c}, median of three runs (derived 1,000,000 / mean µs), now")
    print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
    print("|---|" + "---|" * 6)
    for s in SHAPES:
        print(f"| {s} | " + " | ".join(f"{mq(c, n, m, s):,.0f} qps" for n, m in COLS) + " |")
    print()

print("### cross-version: now over 9a0525d, ratio of median qps, cores = 1 (above 1 = faster now)")
print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
print("|---|" + "---|" * 6)
gm = {k: [] for k in COLS}
for s in SHAPES:
    row = []
    for n, m in COLS:
        r = mq(1, n, m, s) / mq(1, n, m, s, 3)
        gm[(n, m)].append(r)
        row.append(f"{r:.2f}x")
    print(f"| {s} | " + " | ".join(row) + " |")
print("| geometric mean | " + " | ".join(f"{math.exp(sum(math.log(x) for x in gm[k])/len(gm[k])):.3f}x" for k in COLS) + " |")
print()
print("### cross-version, cores = 8")
print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
print("|---|" + "---|" * 6)
gm8 = {k: [] for k in COLS}
for s in SHAPES:
    row = []
    for n, m in COLS:
        r = mq(8, n, m, s) / mq(8, n, m, s, 3)
        gm8[(n, m)].append(r)
        row.append(f"{r:.2f}x")
    print(f"| {s} | " + " | ".join(row) + " |")
print("| geometric mean | " + " | ".join(f"{math.exp(sum(math.log(x) for x in gm8[k])/len(gm8[k])):.3f}x" for k in COLS) + " |")
print()

print("### 9a0525d QPS, cores = 1 (for side-by-side), median of three")
print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
print("|---|" + "---|" * 6)
for s in SHAPES:
    print(f"| {s} | " + " | ".join(f"{mq(1, n, m, s, 3):,.0f} qps" for n, m in COLS) + " |")
print()

print("### cores 8 over cores 1 (now), ratio of median qps")
r8 = []
slower = 0
for s in SHAPES:
    for n, m in COLS:
        r = mq(8, n, m, s) / mq(1, n, m, s)
        r8.append(r)
        slower += r < 1
print(f"min {min(r8):.2f}x max {max(r8):.2f}x median {statistics.median(r8):.2f}x slower in {slower} of {len(r8)}")
r8o = []
for s in SHAPES:
    for n, m in COLS:
        r8o.append(mq(8, n, m, s, 3) / mq(1, n, m, s, 3))
print(f"9a0525d: min {min(r8o):.2f}x max {max(r8o):.2f}x median {statistics.median(r8o):.2f}x slower in {sum(x < 1 for x in r8o)} of {len(r8o)}")
print()

print("### index effect (none over all, mean-latency ratio of medians, cores = 1) now / 9a0525d")
print("| shape | n=200 | n=1,000 | n=10,000 |")
print("|---|---|---|---|")
for s in SHAPES:
    row = []
    for n in SIZES:
        a = statistics.median([ph(load(f"{r}s3-c1-n{n}-none"), s)["mean_us"] for r in REPS])
        b = statistics.median([ph(load(f"{r}s3-c1-n{n}-all"), s)["mean_us"] for r in REPS])
        a0 = statistics.median([ph(load(f"{r}s3-c1-n{n}-none", 3), s)["mean_us"] for r in REPS])
        b0 = statistics.median([ph(load(f"{r}s3-c1-n{n}-all", 3), s)["mean_us"] for r in REPS])
        row.append(f"{a/b:.2f}x / {a0/b0:.2f}x")
    print(f"| {s} | " + " | ".join(row) + " |")
print()

print("### fixed + per-row fit, unindexed, cores 1: mean µs at n=200 and n=10,000 (loans rows), slope ns/row = (m10000-m200)/9800")
print("| shape | mean n=200 | mean n=10,000 | slope | slope 9a0525d |")
print("|---|---|---|---|---|")
for s in ["loans-by-user", "loans-by-book", "loans-by-daterange", "count-by-user", "join-loan-user"]:
    a = statistics.median([ph(load(f"{r}s3-c1-n200-none"), s)["mean_us"] for r in REPS])
    b = statistics.median([ph(load(f"{r}s3-c1-n10000-none"), s)["mean_us"] for r in REPS])
    a0 = statistics.median([ph(load(f"{r}s3-c1-n200-none", 3), s)["mean_us"] for r in REPS])
    b0 = statistics.median([ph(load(f"{r}s3-c1-n10000-none", 3), s)["mean_us"] for r in REPS])
    print(f"| {s} | {a:,.1f} µs | {b:,.1f} µs | {1000*(b-a)/9800:,.0f} ns per row | {1000*(b0-a0)/9800:,.0f} ns per row |")
print()

print("### noise floor: run-to-run spread (max/min - 1) of per-cell mean over three runs; median / worst over shapes")
print("| cores | n | mode | median spread over shapes | worst shape | worst spread |")
print("|---|---|---|---|---|---|")
for c in (1, 8):
    for n, m in COLS:
        sp = []
        for s in SHAPES:
            v = [ph(load(f"{r}s3-c{c}-n{n}-{m}"), s)["mean_us"] for r in REPS]
            sp.append((max(v) / min(v) - 1, s))
        print(f"| {c} | {n:,} | {m} | {100*statistics.median([x for x, _ in sp]):.1f} % | {max(sp)[1]} | {100*max(sp)[0]:.1f} % |")
print()

print("### versus PostgreSQL 18.6 (reused, f2f1ee7), KDS cores = 1 over PG, ratio of median qps")
print("| shape | " + " | ".join(f"n={n:,} {m}" for n, m in COLS) + " |")
print("|---|" + "---|" * 6)
g = {k: [] for k in COLS}
ahead = {k: 0 for k in COLS}
for s in SHAPES:
    row = []
    for i, (n, m) in enumerate(COLS):
        r = mq(1, n, m, s) / PG[s][i]
        g[(n, m)].append(r)
        ahead[(n, m)] += r > 1.05
        row.append(f"{r:.2f}x")
    print(f"| {s} | " + " | ".join(row) + " |")
print("| geometric mean (shapes ahead by >1.05x) | " + " | ".join(f"{math.exp(sum(math.log(x) for x in g[k])/len(g[k])):.2f}x ({ahead[k]} of 12)" for k in COLS) + " |")
print()
print("### PG18 qps (reused), for the summary")
for s in SHAPES:
    print(f"| {s} | " + " | ".join(f"{x:,.0f} qps" for x in PG[s]) + " |")
print()

print("### one-shot phases (cores = 1, run 1)")
print("| n | mode | load ops | load p50 | load mean | create-index ops | create-index p50 | create-index mean | now/9a0525d load mean |")
print("|---|---|---|---|---|---|---|---|---|")
for n in SIZES:
    for m in MODES:
        d = load(f"s3-c1-n{n}-{m}")
        o = load(f"s3-c1-n{n}-{m}", 3)
        l = ph(d, "load")
        lo = ph(o, "load")
        ci = ph(d, "create-index")
        cio = ph(o, "create-index")
        cis = (f"{ci['ops']} ops | {ci['p50_us']:,.0f} µs | {ci['mean_us']:,.0f} µs") if ci else "not run | - | -"
        cmp = f"{l['mean_us']/lo['mean_us']:.2f}x" + (f"; create-index {ci['mean_us']/cio['mean_us']:.2f}x" if ci and cio else "")
        print(f"| {n:,} | {m} | {l['ops']:,} ops | {l['p50_us']:,.0f} µs | {l['mean_us']:,.0f} µs | {cis} | {cmp} |")
print()

for c in (1, 8):
    print(f"### percentiles, cores = {c}, run 1 (200 ops per shape)")
    print("| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan |")
    print("|---|---|---|---|---|---|---|---|---|")
    for n in SIZES:
        for m in MODES:
            d = load(f"s3-c{c}-n{n}-{m}")
            plans = d["meta"]["plans"]
            for s in SHAPES:
                p = ph(d, s)
                pl = ",".join(plans.get(s, ["-"]))
                print(f"| {n:,} | {m} | {s} | {fmt_us(p['p0_us'])} | {fmt_us(p['p25_us'])} | {fmt_us(p['p50_us'])} | {fmt_us(p['p95_us'])} | {fmt_us(p['p99_us'])} | {pl} |")
    print()

print("### host stamps + validity")
starts, ends = timeline_windows()
samples = monitor_samples()
print("| cell | precheck UTC | loadavg | gate wait | build procs | driver exit | verify problems | samples | build/test | heavy | minor |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for r in REPS:
    for c in (1, 8):
        for n in SIZES:
            for m in MODES:
                tag = f"{r}s3-c{c}-n{n}-{m}"
                tm, la, gw, pr = precheck(tag)
                rc = open(f"{L}/{tag}.rc").read().strip()
                vp = len(load(tag)["meta"]["verify_problems"])
                nn, b, h, mi, det = validity(tag, samples, ts(tm), ts(ends[tag][0]))
                print(f"| `{tag}` | {tm} | {la} | {gw} s | {pr} | exit {rc} | {vp} problems | {nn} | {b} | {h} | {mi} |")
