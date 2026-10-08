#!/usr/bin/env python3
"""Where the hot rows that survived the scan sit in the warm (fault) order.

usage: survivors.py JSON
Per variant: survivors (rows whose `after` lookup missed nothing) per tenth
of the warm order, and the longest run of consecutive survivors.
"""
import json, sys

d = json.load(open(sys.argv[1]))
for v in d["variants"]:
    lost = v["after_lost"]
    n = len(lost)
    surv = [i for i, x in enumerate(lost) if x == 0]
    deciles = [0] * 10
    for i in surv:
        deciles[i * 10 // n] += 1
    runs, cur = [], 0
    for x in lost:
        cur = cur + 1 if x == 0 else 0
        runs.append(cur)
    first = surv[:5]
    last = surv[-5:]
    print(f"hot={v['hot_pages']} warm={v['warm_passes']} laps={v['scan']['laps']} "
          f"survivors={len(surv)} per-tenth={deciles} longest-run={max(runs) if runs else 0} "
          f"first={first} last={last}")
    # The miss count per lookup: 1 = the leaf alone, 2+ = leaf and internal pages.
    hist = {}
    for x in lost:
        hist[x] = hist.get(x, 0) + 1
    print("   misses-per-lookup histogram:", dict(sorted(hist.items())))
