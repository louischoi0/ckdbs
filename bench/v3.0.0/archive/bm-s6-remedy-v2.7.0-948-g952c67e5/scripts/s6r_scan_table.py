#!/usr/bin/env python3
"""BM-S6 remedy: the bn scan cell of tools/bm_scan.py, arms A and B2.

    s6r_scan_table.py BMSCAN.json --section net|pct|hosts|digest|keys
"""
import collections, json, statistics, sys

med = statistics.median


def rng(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def nets(runs, path):
    out = collections.OrderedDict()
    for run in runs:
        point = {}
        for x in run["arms"]:
            if x["kind"] == "point" and x["path"] == path:
                point[(x["arm"], x["rel"])] = x["server_cpu_us"]
        for x in run["arms"]:
            if x["kind"] == "scan" and x["path"] == path:
                out.setdefault(x["arm"], []).append((x["server_cpu_us"] - point[(x["arm"], x["rel"])]) * 1e3 / x["rows"])
    return out


def t_net(runs):
    print("| path | A net ns/row | B2 net ns/row | A rows/s (derived) | B2 rows/s (derived) | B2 - A (paired by run) | B2 slower than A |")
    print("|---|---|---|---|---|---|---|")
    for path in ("literal", "prepared"):
        n = nets(runs, path)
        d = [b - a for a, b in zip(n["A"], n["B2"])]
        print("| %s | %s ns | %s ns | %s /s | %s /s | %s ns (%+.1f %%) | %d of %d |" % (
            path, rng(n["A"]), rng(n["B2"]),
            format(round(1e9 / med(n["A"])), ","), format(round(1e9 / med(n["B2"])), ","),
            rng(d, "%+.1f"), 100.0 * med(d) / med(n["A"]), sum(1 for v in d if v > 0), len(d)))
    print("\nper run, literal net ns/row:\n")
    n = nets(runs, "literal")
    print("| run | A | B2 | B2 - A |")
    print("|---|---|---|---|")
    for i in range(len(runs)):
        print("| %d | %.1f | %.1f | %+.1f |" % (i, n["A"][i], n["B2"][i], n["B2"][i] - n["A"][i]))


def t_pct(runs):
    print("| path | arm | n (scans, all runs) | p0 | p25 | p50 | p90 | p99 | scans/s at p50 (derived) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for path in ("literal", "prepared"):
        for arm in ("A", "B2"):
            xs = [x for r in runs for x in r["arms"] if x["kind"] == "scan" and x["path"] == path and x["arm"] == arm]
            m = lambda k: med([x[k] for x in xs])
            print("| %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %s /s |" % (
                path, arm, format(sum(x["ops"] for x in xs), ","), m("p_p0"), m("p_p25"), m("p_p50"), m("p_p90"), m("p_p99"),
                format(round(1e6 / m("p_p50")), ",")))


def t_hosts(runs):
    print("| run | 1-min load at the run gate | gate wait | load before / after |")
    print("|---|---|---|---|")
    for r in runs:
        print("| %d | %.2f | %.0f s | %s / %s |" % (r["run"], r["quiet_gate"]["load"], r["quiet_gate"]["waited_s"],
                                                  r["host_before"]["loadavg"].split()[0], r["host_after"]["loadavg"].split()[0]))
    hs = []
    for r in runs:
        seen = set()
        for x in r["arms"]:
            h = x.get("host")
            k = (x["cell"], x.get("path"))
            if h is None or k in seen:
                continue
            seen.add(k)
            hs.append(h)
    print("\ncell-runs: %d; clean: %d; above the load gate (2.0): %d; with a competing build or test run: %d; bad replies: %d" % (
        len(hs), sum(1 for h in hs if h.get("clean")), sum(1 for h in hs if float(h["loadavg"].split()[0]) > 2.0),
        sum(1 for h in hs if h["competing"]), sum(x.get("bad", 0) for r in runs for x in r["arms"])))
    print("cell load range: %.2f - %.2f" % (min(float(h["loadavg"].split()[0]) for h in hs), max(float(h["loadavg"].split()[0]) for h in hs)))


def t_keys(runs):
    print(sorted(runs[0].keys()))
    print(sorted(runs[0]["arms"][-1].keys()))
    for k in runs[0]:
        if k not in ("arms", "host_before", "host_after"):
            print(k, json.dumps(runs[0][k])[:600])


if __name__ == "__main__":
    runs = json.load(open(sys.argv[1]))["runs"]
    {"net": t_net, "pct": t_pct, "hosts": t_hosts, "keys": t_keys}[sys.argv[sys.argv.index("--section") + 1]](runs)
