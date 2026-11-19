#!/usr/bin/env python3
"""Per (cell, arm): median over blocks; and round-paired deltas of server CPU and p50
against a baseline arm, with a bootstrap 95 % interval of the median paired delta.
usage: an.py out.json BASE [pairs like X-A,Y-X ...]"""
import collections, json, random, statistics, sys
out = json.load(open(sys.argv[1])); base = sys.argv[2]
pairs = [tuple(x.split("-")) for x in sys.argv[3].split(",")] if len(sys.argv) > 3 else []
d = collections.defaultdict(dict)
for r in out: d[(r["cell"], r["engine"])][r["round"]] = r
cells = list(dict.fromkeys(r["cell"] for r in out)); engs = list(dict.fromkeys(r["engine"] for r in out))
med = lambda rs, k: statistics.median(x[k] for x in rs.values())
def paired(c, e, b, k):
    rounds = sorted(set(d[(c, e)]) & set(d[(c, b)]))
    xs = [d[(c, e)][r][k] - d[(c, b)][r][k] for r in rounds]
    rng = random.Random(f"{c}|{e}|{b}|{k}")  # a pair's interval does not depend on which others are asked
    bs = sorted(statistics.median(rng.choices(xs, k=len(xs))) for _ in range(2000))
    return statistics.median(xs), bs[50], bs[1949], len(xs)
print("cell,arm,blocks,n,qps,p0,p25,p50,p95,p99,cpu_us,maxload,competing")
for c in cells:
    for e in engs:
        rs = d[(c, e)]
        ml = max(float(x["host"]["loadavg"].split()[0]) for x in rs.values())
        comp = sum(1 for x in rs.values() if x["host"]["competing"])
        print(f'{c},{e},{len(rs)},{sum(x["ops"] for x in rs.values())},{med(rs,"qps"):.0f},{med(rs,"p0"):.1f},{med(rs,"p25"):.1f},'
              f'{med(rs,"p50"):.1f},{med(rs,"p95"):.1f},{med(rs,"p99"):.1f},{med(rs,"server_cpu_us"):.2f},{ml},{comp}')
print(f"\nround-paired vs {base}: cpu us median [95% CI] | p50 us median [CI] | qps %")
for c in cells:
    for e in engs:
        if e == base: continue
        m, lo, hi, n = paired(c, e, base, "server_cpu_us"); pm, plo, phi, _ = paired(c, e, base, "p50")
        q = (med(d[(c, e)], "qps") / med(d[(c, base)], "qps") - 1) * 100
        print(f"{c},{e},cpu {m:+.2f} [{lo:+.2f},{hi:+.2f}] | p50 {pm:+.2f} [{plo:+.2f},{phi:+.2f}] | qps {q:+.1f}% n={n}")
if pairs:
    print("\nround-paired steps: cpu us median [95% CI] | p50")
    for c in cells:
        for e, b in pairs:
            m, lo, hi, n = paired(c, e, b, "server_cpu_us"); pm, plo, phi, _ = paired(c, e, b, "p50")
            print(f"{c},{e}-{b},cpu {m:+.2f} [{lo:+.2f},{hi:+.2f}] | p50 {pm:+.2f} [{plo:+.2f},{phi:+.2f}]")
