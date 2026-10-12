#!/usr/bin/env python3
"""BL-S3 convergence: arm D's skew under the balancer against the same skew
without it and a balanced placement without it. Sessions move under the
balancer, so nothing here is attributed to a core: per arm, throughput, the
pooled p99, the worst client's p99 and the client p99 spread (max/min), and
the moves the cores counted. Usage: analyze_converge.py DIR"""
import collections, glob, json, statistics, sys

rows = collections.defaultdict(list)
for f in glob.glob(sys.argv[1] + "/*/r*/*.json"):
    arm = f.split("/")[-3]
    d = json.load(open(f))
    rows[(d["cores"], d["sessions"], arm, d["placement"])].append(d)

print(f"{'cores':>5} {'sess':>4} {'binary':6} {'placement':9} {'n':>2} {'stmt/s':>16} {'p99':>6} "
      f"{'worst client p99':>17} {'client spread':>13} {'moves':>6}")
for key in sorted(rows):
    rs = rows[key]
    tp = [d["statements_per_s"] for d in rs]
    worst = [max(c["p99_us"] for c in d["by_client"]) for d in rs]
    spread = [max(c["p99_us"] for c in d["by_client"]) / min(c["p99_us"] for c in d["by_client"]) for d in rs]
    moves = [sum(v for k, v in d["meta_delta"].items() if k.endswith("_moves_out")) for d in rs]
    print(f"{key[0]:5} {key[1]:4} {key[2]:6} {key[3]:9} {len(rs):2} "
          f"{statistics.mean(tp):9.0f}±{(statistics.stdev(tp) if len(tp) > 1 else 0):5.0f} "
          f"{statistics.median(d['p99_us'] for d in rs):6.0f} {statistics.median(worst):17.0f} "
          f"{statistics.median(spread):13.2f} {statistics.median(moves):6.0f}")
