#!/usr/bin/env python3
"""Pools per-run corr-exists-scan diffs of two result dirs (B'-A) and prints median/IQR/sign counts, plus a bootstrap CI of the median."""
import glob, json, math, random, re, statistics, sys

def q(vals, p):
    s = sorted(vals); k = (len(s) - 1) * p / 100; f = math.floor(k); c = math.ceil(k)
    return s[f] + (s[c] - s[f]) * (k - f)

def diffs(d, swap=False):
    out = {}
    for f in glob.glob(f"{d}/n*-run*.json"):
        n = int(re.match(r".*/n(\d+)-run", f).group(1))
        j = json.load(open(f))
        for arm in j["blocks"]["A"]:
            if arm != "corr-exists-scan":
                continue
            la = [x for b in j["blocks"]["A"][arm] for x in b["lat_us"]]
            lb = [x for b in j["blocks"]["B"][arm] for x in b["lat_us"]]
            dd = statistics.median(lb) - statistics.median(la)
            out.setdefault(n, []).append(-dd if swap else dd)
    return out

main = diffs(sys.argv[1]); ext = diffs(sys.argv[2])
random.seed(1)
for n in sorted(main):
    v = main[n] + ext[n]
    boots = sorted(statistics.median(random.choices(v, k=len(v))) for _ in range(5000))
    print(f"{n:>6} pooled runs={len(v)} median={statistics.median(v):+8.2f} us IQR[{q(v,25):+8.2f},{q(v,75):+8.2f}] "
          f"B>A {sum(x>0 for x in v)}/{len(v)} ns/row={statistics.median(v)*1000/n:+6.1f} "
          f"boot95 median [{boots[125]*1000/n:+6.1f},{boots[4875]*1000/n:+6.1f}] ns/row")
