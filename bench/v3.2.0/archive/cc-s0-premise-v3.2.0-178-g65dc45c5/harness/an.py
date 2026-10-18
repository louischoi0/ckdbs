#!/usr/bin/env python3
import json, statistics, sys, collections
path = sys.argv[1]
R = json.load(open(path))
cells = list(dict.fromkeys(r["cell"] for r in R))
arms = ["P:A", "P:B", "W:A", "W:B", "W2:A"]


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * q))] if q < 1 else xs[-1]


out = {}
for c in cells:
    out[c] = {}
    for arm in arms:
        rs = [r for r in R if r["cell"] == c and r["arm"] == arm]
        if not rs: continue
        cpu = [r["server_cpu_us"] for r in rs]
        lat = [x for r in rs for x in r["lat_us"]]
        qps = [r["qps"] for r in rs]
        med = statistics.median(cpu)
        out[c][arm] = {"n_rounds": len(rs), "ops_per_round": rs[0]["ops"], "cpu_per_round": [round(x, 1) for x in cpu],
                       "cpu_median": med, "cpu_spread_pct": (max(cpu) - min(cpu)) / med * 100,
                       "qps_median": statistics.median(qps),
                       "lat": {k: pct(lat, q) for k, q in (("p0", 0), ("p25", .25), ("p50", .5), ("p95", .95), ("p99", .99))},
                       "n_ops": len(lat), "rows": rs[0]["rows_total"],
                       "loadavg": [r["host"]["loadavg1"] for r in rs], "foreign": sum(r["host"]["foreign"] for r in rs)}
json.dump(out, open(path + ".summary", "w"), indent=1)
for c in cells:
    print(c)
    base = out[c]["W:A"]["cpu_median"]
    for arm in arms:
        if arm not in out[c]: continue
        a = out[c][arm]
        print(f"  {arm:5s} cpu {a['cpu_median']:9.1f} us  d_vs_W:A {100*(a['cpu_median']/base-1):+6.1f}%  spread {a['cpu_spread_pct']:5.1f}%  "
              f"qps {a['qps_median']:9.1f}  p0 {a['lat']['p0']:.0f} p25 {a['lat']['p25']:.0f} p50 {a['lat']['p50']:.0f} p95 {a['lat']['p95']:.0f} p99 {a['lat']['p99']:.0f}  rounds {a['cpu_per_round']}")
