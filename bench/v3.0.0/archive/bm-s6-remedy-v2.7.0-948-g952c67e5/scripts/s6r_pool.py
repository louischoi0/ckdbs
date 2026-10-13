#!/usr/bin/env python3
"""Pool the every-row UPDATE runs of several bn_census files (same arms A, A2, B, B2).

    s6r_pool.py FILE1.json,FILE2.json,... --section guard|batches|pct
"""
import collections, json, statistics, sys

med = statistics.median
LABEL = {"A": "A", "A2": "A'", "B": "B", "B2": "B2"}


def load(files):
    out = []
    for i, f in enumerate(files):
        for r in json.load(open(f))["runs"]:
            r["_batch"] = i + 1
            out.append(r)
    return out


def rng(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def per(runs, shape, path):
    by = collections.OrderedDict((a, []) for a in ("A", "A2", "B", "B2"))
    batch = []
    for r in runs:
        for x in r["arms"]:
            if x["shape"] == shape and x["path"] == path:
                by[x["bin"]].append(x["server_cpu_us"])
        batch.append(r["_batch"])
    return by, batch


def t_guard(runs):
    n = len(runs)
    print("| path | A | A' | B | B2 | B - A | B2 - A | B2 - B | A - A' (S = max abs; typical = median abs) |")
    print("|---|---|---|---|---|---|---|---|---|")
    sign = []
    for path in ("literal", "prepared"):
        by, _ = per(runs, "update-range", path)
        d = lambda x, y: [p - q for p, q in zip(by[x], by[y])]
        f = lambda dd: "%s (%+.1f %%)" % (rng(dd, "%+.2f"), 100.0 * med(dd) / med(by["A"]))
        daa = d("A", "A2")
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s (S = %.2f, typical %.2f) |" % (
            path, rng(by["A"], "%.2f"), rng(by["A2"], "%.2f"), rng(by["B"], "%.2f"), rng(by["B2"], "%.2f"),
            f(d("B", "A")), f(d("B2", "A")), f(d("B2", "B")), rng(daa, "%+.2f"), max(abs(v) for v in daa), med([abs(v) for v in daa])))
        gt = lambda dd: "%d of %d" % (sum(1 for v in dd if v > 0), len(dd))
        sign.append((path, gt(d("B2", "A")), gt(d("B", "A")), gt(d("B2", "B")), gt(daa), gt([-v for v in daa])))
    print("\nruns in which the first arm is slower than the second (%d paired runs):\n" % n)
    print("| path | B2 slower than A | B slower than A | B2 slower than B | A slower than A' | A' slower than A |")
    print("|---|---|---|---|---|---|")
    for s in sign:
        print("| %s | %s | %s | %s | %s | %s |" % s)


def t_batches(runs):
    print("| batch (server start order) | path | runs | A | A' | B | B2 | B - A | B2 - A | B2 - B | A - A' typical |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    orders = {1: "A, A', B, B2", 2: "B2, B, A', A", 3: "B, B2, A, A'"}
    for b in sorted(set(r["_batch"] for r in runs)):
        rs = [r for r in runs if r["_batch"] == b]
        for path in ("literal", "prepared"):
            by, _ = per(rs, "update-range", path)
            d = lambda x, y: [p - q for p, q in zip(by[x], by[y])]
            print("| %d (%s) | %s | %d | %.1f | %.1f | %.1f | %.1f | %+.1f | %+.1f | %+.1f | %.1f |" % (
                b, orders.get(b, "?"), path, len(rs), med(by["A"]), med(by["A2"]), med(by["B"]), med(by["B2"]),
                med(d("B", "A")), med(d("B2", "A")), med(d("B2", "B")), med([abs(v) for v in d("A", "A2")])))


def t_pct(runs):
    print("| path | arm | n (statements, all runs) | p0 | p25 | p50 | p90 | p99 | statements/s at p50 (derived) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for path in ("literal", "prepared"):
        for lab in ("A", "A2", "B", "B2"):
            xs = [x for r in runs for x in r["arms"] if x["shape"] == "update-range" and x["path"] == path and x["bin"] == lab]
            m = lambda k: med([x[k] for x in xs])
            print("| %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %s /s |" % (
                path, LABEL[lab], format(sum(x["ops"] for x in xs), ","), m("p0_us"), m("p25_us"), m("p50_us"), m("p90_us"), m("p99_us"),
                format(round(1e6 / m("p50_us")), ",")))


if __name__ == "__main__":
    runs = load(sys.argv[1].split(","))
    {"guard": t_guard, "batches": t_batches, "pct": t_pct}[sys.argv[sys.argv.index("--section") + 1]](runs)
