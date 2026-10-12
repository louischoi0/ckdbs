#!/usr/bin/env python3
"""BL-Q3: which per-core signal predicts the per-core p99. For every run of
the skew cells with mid-span gauges, each core is one point: its signal value
and the median p99 of the clients on it. Reported per signal: the rank
correlation with p99 within each run, averaged over runs (a signal must order
the cores of one run as their p99 does - the balancer compares cores, not
runs). Usage: analyze_signal.py DIR [DIR...]"""
import collections, glob, json, statistics, sys

SIGNALS = ("sessions", "busy_ppm", "foreground_ppm", "queued_foreground")


def ranks(xs):
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    r = [0.0] * len(xs)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2
        i = j + 1
    return r


def spearman(a, b):
    if len(a) < 2 or len(set(a)) < 2 or len(set(b)) < 2:
        return None
    ra, rb = ranks(a), ranks(b)
    ma, mb = statistics.mean(ra), statistics.mean(rb)
    num = sum((x - ma) * (y - mb) for x, y in zip(ra, rb))
    den = (sum((x - ma) ** 2 for x in ra) * sum((y - mb) ** 2 for y in rb)) ** 0.5
    return num / den if den else None


per = collections.defaultdict(lambda: collections.defaultdict(list))
for d in sys.argv[1:]:
    for f in glob.glob(d + "/r*/*.json"):
        r = json.load(open(f))
        g = r.get("meta_gauges_mid") or {}
        if not g:
            continue
        by = collections.defaultdict(list)
        for c in r["by_client"]:
            if c.get("p99_us") is not None:
                by[int(c["core"])].append(c["p99_us"])
        cores = sorted(by)
        p99 = [statistics.median(by[k]) for k in cores]
        cell = (r["cores"], r["sessions"], r["durability"], r["shape"])
        for s in SIGNALS:
            vals = [g.get(f"load_c{k}_{s}", 0.0) for k in cores]
            rho = spearman(vals, p99)
            if rho is not None:
                per[cell][s].append(rho)

for cell in sorted(per):
    print(cell, {s: (round(statistics.mean(v), 2), len(v)) for s, v in per[cell].items()})
allsig = collections.defaultdict(list)
for cell in per:
    for s, v in per[cell].items():
        allsig[s] += v
print("ALL", {s: (round(statistics.mean(v), 2), len(v)) for s, v in allsig.items()})
