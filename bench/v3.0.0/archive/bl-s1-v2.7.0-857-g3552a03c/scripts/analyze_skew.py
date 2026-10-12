#!/usr/bin/env python3
"""Summarize BL-S1 skew cells: per (cores, sessions, durability) and arm,
throughput mean/sd over runs, pooled p50/p99, per-core p99 spread and the
session distribution. Usage: analyze_skew.py DIR"""
import collections, glob, json, statistics, sys

rows = collections.defaultdict(list)
for f in glob.glob(sys.argv[1] + "/r*/*.json"):
    d = json.load(open(f))
    rows[(d["cores"], d["sessions"], d["durability"], d["placement"])].append(d)


def per_core(d):
    by = collections.defaultdict(list)
    for c in d["by_client"]:
        by[int(c["core"])].append(c)
    return by


def spread(d):
    by = per_core(d)
    p99 = [statistics.median(c["p99_us"] for c in v) for v in by.values() if v]
    return max(p99) / min(p99) if len(p99) > 1 else 1.0, max(p99)


def ms(xs):
    return (statistics.mean(xs), statistics.stdev(xs) if len(xs) > 1 else 0.0)


print("cores sess dur      arm       n  stmt/s(mean±sd)     p50   p99   worst-core-p99  p99-spread  dist(sample)  cbound")
for key in sorted(rows):
    rs = rows[key]
    tp = ms([d["statements_per_s"] for d in rs])
    p50 = statistics.median(d["p50_us"] for d in rs)
    p99 = statistics.median(d["p99_us"] for d in rs)
    sp = [spread(d) for d in rs]
    dist = sorted(collections.Counter(int(c) for c in rs[0]["session_cores"]).values(), reverse=True)
    cb = sum(d["client_bound"] for d in rs)
    print(f"{key[0]:5} {key[1]:4} {key[2]:8} {key[3]:9} {len(rs):2} {tp[0]:9.0f}±{tp[1]:6.0f} {p50:7.0f} {p99:6.0f} "
          f"{statistics.median(s[1] for s in sp):10.0f} {statistics.median(s[0] for s in sp):10.2f}  {dist}  {cb}")
