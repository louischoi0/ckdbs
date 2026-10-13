#!/usr/bin/env python3
"""BT-S1 results tables beyond table.py: gate reading, additivity, pooled scan
latency percentiles, the scan statement's CPU/wall decomposition, wait counters,
identity checks, host per run.  Reads bmscan-bn.json (or .json.gz).  Tables only."""
import collections, gzip, json, re, statistics, sys

med = statistics.median
path = sys.argv[1]
d = json.load(gzip.open(path, "rt") if path.endswith(".gz") else open(path))
runs = d["runs"]
ARMS = ["A", "A2", "D1", "D2", "D3", "all", "D4"]
PATHS = ("literal", "prepared")


def cells(kind, p):
    out = collections.defaultdict(list)          # arm -> [cell per run]
    for r in runs:
        for x in r["arms"]:
            if x["kind"] == kind and x["path"] == p:
                out[x["arm"]].append(x)
    return out


def net(p):
    sc, pt = cells("scan", p), cells("point", p)
    return {a: [(sc[a][i]["server_cpu_us"] - pt[a][i]["server_cpu_us"]) * 1e3 / sc[a][i]["rows"]
                for i in range(len(runs))] for a in ARMS}


def pct(xs, q):
    xs = sorted(xs)
    k = (len(xs) - 1) * q / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


print("## gate reading (ns/row; paired by run against A; negative = faster)")
for p in PATHS:
    n = net(p)
    gap = [b - a for a, b in zip(n["A"], n["A2"])]
    print("\n%s: A2 - A per run %s; median abs gap %.2f, max abs gap %.2f" % (
        p, ["%+.1f" % g for g in gap], med([abs(g) for g in gap]), max(abs(g) for g in gap)))
    sav = {a: [x - y for x, y in zip(n["A"], n[a])] for a in ARMS}   # positive = saved
    print("| arm | saved vs A (median) | min..max | runs saving >= 1.0 | runs saving >= 3.0 |")
    for a in ("D1", "D2", "D3", "all", "D4"):
        s = sav[a]
        print("| %s | %.1f | %.1f..%.1f | %d of 5 | %d of 5 |" % (
            a, med(s), min(s), max(s), sum(1 for v in s if v >= 1.0), sum(1 for v in s if v >= 3.0)))
    d4all = [x - y for x, y in zip(n["all"], n["D4"])]
    print("D4 below all (paired): median %.2f [%.2f, %.2f], runs D4<all %d of 5" % (
        med(d4all), min(d4all), max(d4all), sum(1 for v in d4all if v > 0)))
    parts = med(sav["D1"]) + med(sav["D2"]) + med(sav["D3"])
    print("D1+D2+D3 median savings %.1f vs all %.1f (overlap %.1f)" % (parts, med(sav["all"]), parts - med(sav["all"])))
    allv = [x - y for x, y in zip(n["A"], n["all"])]
    print("net as share of A: all saves %.1f %%, D4 saves %.1f %%" % (
        100 * med(sav["all"]) / med(n["A"]), 100 * med(sav["D4"]) / med(n["A"])))
    print("rows/s (derived 1e9/ns): " + ", ".join("%s %s" % (a, format(int(1e9 / med(n[a])), ",")) for a in ARMS))

print("\n## scan statement latency, pooled over the 5 runs (192 scans per run), linear-interpolated percentiles, us")
print("| path | arm | n | p0 | p25 | p50 | p90 | p95 | p99 | mean |")
for p in PATHS:
    sc = cells("scan", p)
    for a in ARMS:
        lat = [v for c in sc[a] for v in c["lat_us"]]
        print("| %s | %s | %d | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |" % (
            p, a, len(lat), min(lat), pct(lat, 25), pct(lat, 50), pct(lat, 90), pct(lat, 95), pct(lat, 99),
            sum(lat) / len(lat)))

print("\n## scan latency percentiles as the driver reports them: median over runs of each run's percentile, us")
print("| path | arm | p0 | p25 | p50 | p90 | p95 | p99 |")
for p in PATHS:
    sc = cells("scan", p)
    for a in ARMS:
        print("| %s | %s | " % (p, a) + " | ".join("%.1f" % med([c["p_" + k] for c in sc[a]])
              for k in ("p0", "p25", "p50", "p90", "p95", "p99")) + " |")

print("\n## decomposition of the scan statement (medians over 5 runs, us)")
print("| path | arm | wall mean | scan server CPU | point server CPU | rows x net | driver CPU | wall - server CPU |")
for p in PATHS:
    sc, pt = cells("scan", p), cells("point", p)
    n = net(p)
    for a in ARMS:
        wall = med([c["p_mean"] for c in sc[a]])
        scpu = med([c["server_cpu_us"] for c in sc[a]])
        pcpu = med([c["server_cpu_us"] for c in pt[a]])
        dcpu = med([c["driver_cpu_us"] for c in sc[a]])
        print("| %s | %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |" % (
            p, a, wall, scpu, pcpu, med(n[a]) * 10, dcpu, wall - scpu))

print("\n## wait counters (sum over 5 runs; scan cells, then point cells)")
for kind in ("scan", "point"):
    for p in PATHS:
        c = cells(kind, p)
        print(kind, p, {a: {k: sum(x[k] for x in c[a]) for k in ("lookups", "lookups_instance", "window_wait_us", "pool_misses", "bad")}
                        for a in ARMS})
        print("   pool_misses per run (A):", [x["pool_misses"] for x in c["A"]])

print("\n## identity: run 0 records ANALYZE and the digest")
r0 = runs[0]
print("digest:", r0["digest"])
for a in ARMS:
    v = r0["analyze"]["bn"][a]
    print(a, re.search(r"examined=\d+ pages=\d+", v).group(0), "|", re.search(r"step 0 FilterScan s opens=.*", v).group(0))
print("rows per scan cell:", sorted({x["rows"] for r in runs for x in r["arms"] if x["kind"] == "scan"}),
      "bad:", sum(x["bad"] for r in runs for x in r["arms"]))

print("\n## host per run")
for r in runs:
    hs = [x["host"] for x in r["arms"]]
    print("run %d gate waited %.0f s at load %.2f; cells clean %s; load at cells %.2f-%.2f; sibling busy max %.1f %%; foreign srv/drv max %.1f/%.1f %%; competing %s; attempts max %d" % (
        r["run"], r["quiet_gate"]["waited_s"], r["quiet_gate"]["load"], all(h["clean"] for h in hs),
        min(float(h["loadavg"].split()[0]) for h in hs), max(float(h["loadavg"].split()[0]) for h in hs),
        max(h["sibling_busy_pct"] for h in hs), max(h["foreign_server_cpu_pct"] for h in hs),
        max(h["foreign_driver_cpu_pct"] for h in hs), any(h["competing"] for h in hs), max(h["attempts"] for h in hs)))
