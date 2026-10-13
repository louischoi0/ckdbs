#!/usr/bin/env python3
"""BT-S1: net ns/row per arm (scan - point, per row), paired deltas against A."""
import collections, json, statistics, sys

med = statistics.median
runs = json.load(open(sys.argv[1]))
runs = runs["runs"] if isinstance(runs, dict) and "runs" in runs else runs
ARMS = ["A", "A2", "D1", "D2", "D3", "all", "D4"]


def nets(path):
    out = collections.OrderedDict()
    for run in runs:
        point = {}
        for x in run["arms"]:
            if x["kind"] == "point" and x["path"] == path:
                point[(x["arm"], x["rel"])] = x["server_cpu_us"]
        for x in run["arms"]:
            if x["kind"] == "scan" and x["path"] == path:
                out.setdefault(x["arm"], []).append(
                    (x["server_cpu_us"] - point[(x["arm"], x["rel"])]) * 1e3 / x["rows"])
    return out


for path in ("literal", "prepared"):
    n = nets(path)
    print("\n%s: net ns/row median [min, max]; delta vs A paired by run median [min, max], runs below A" % path)
    print("| arm | net ns/row | arm - A | below A |")
    print("|---|---|---|---|")
    for arm in ARMS:
        xs = n[arm]
        d = [b - a for a, b in zip(n["A"], xs)]
        print("| %s | %.1f [%.1f, %.1f] | %+.1f [%+.1f, %+.1f] | %d of %d |" % (
            arm, med(xs), min(xs), max(xs), med(d), min(d), max(d), sum(1 for v in d if v < 0), len(d)))
    print("\nper run:", path)
    print("| run | " + " | ".join(ARMS) + " |")
    print("|---" * (len(ARMS) + 1) + "|")
    for i in range(len(runs)):
        print("| %d | " % i + " | ".join("%.1f" % n[a][i] for a in ARMS) + " |")
