#!/usr/bin/env python3
"""Tables from a `tools/bn_census.py` output file (BN-S1, BN-S8).

    bn_census_table.py OUT.json

Per (shape, path): each binary's median over runs of p0/p25/p50/p90/p99,
the mean, the server's and the driver's CPU per statement. With two binaries
(`--bin-b`), B - A per run and its median and min..max over runs - the
run-to-run spread BN-Q1's bar compares a predicted win against - for p50
and for the server's CPU.
"""
import json, statistics, sys


def main():
    data = json.load(open(sys.argv[1]))
    runs = data["runs"]
    by = {}
    for run in runs:
        for arm in run["arms"]:
            by.setdefault((arm["shape"], arm["path"]), {}).setdefault(arm["bin"], []).append(arm)
    two = any("B" in v for v in by.values())
    med = lambda xs, k: statistics.median(x[k] for x in xs)
    print("| shape | path | bin | p0 | p25 | p50 | p90 | p99 | mean | server CPU | driver CPU |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for (shape, path), sides in by.items():
        for label in sorted(sides):
            xs = sides[label]
            print("| %s | %s | %s | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f |" % (
                shape, path, label, med(xs, "p0_us"), med(xs, "p25_us"), med(xs, "p50_us"),
                med(xs, "p90_us"), med(xs, "p99_us"), med(xs, "mean_us"),
                med(xs, "server_cpu_us"), med(xs, "driver_cpu_us")))
    if two:
        print("\n| shape | path | A p50 | B - A p50, median [min, max] over runs | "
              "A server CPU | B - A server CPU, median [min, max] |")
        print("|---|---|---|---|---|---|")
        for (shape, path), sides in by.items():
            a, b = sides.get("A", []), sides.get("B", [])
            if not a or len(a) != len(b):
                continue
            d50 = [y["p50_us"] - x["p50_us"] for x, y in zip(a, b)]
            dcpu = [y["server_cpu_us"] - x["server_cpu_us"] for x, y in zip(a, b)]
            print("| %s | %s | %.1f | %+.2f [%+.2f, %+.2f] | %.1f | %+.2f [%+.2f, %+.2f] |" % (
                shape, path, med(a, "p50_us"), statistics.median(d50), min(d50), max(d50),
                med(a, "server_cpu_us"), statistics.median(dcpu), min(dcpu), max(dcpu)))
    for key in ("pages-unspilled", "pages-spilled", "end_state", "idle_cpu_us_per_s"):
        print("\n%s: %s" % (key, json.dumps([run.get(key) for run in runs])))
    bad = sum(arm["bad"] for run in runs for arm in run["arms"])
    print("\nbad replies: %d" % bad)


if __name__ == "__main__":
    main()
