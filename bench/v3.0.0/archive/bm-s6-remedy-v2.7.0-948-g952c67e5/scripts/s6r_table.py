#!/usr/bin/env python3
"""Tables for BM-S6's remedy re-measure (arms A, A2, B, B2 of tools/bn_census.py).

    s6r_table.py bn BN.json --section guard|runs|pct|hosts

A is BM's opening commit, A2 the same bytes started as a second server (the
in-run noise control), B the BM-S4 commit 6b771ec1, B2 the remedy commit
952c67e5. Same bar as tools/bm_close_table.py: the spread S is the largest
|A_r - A2_r| over runs, the typical gap the median of |A_r - A2_r|.
"""
import collections, json, statistics, sys

med = statistics.median
LABEL = {"A": "A", "A2": "A'", "B": "B", "B2": "B2"}
ORDER = ("A", "A2", "B", "B2")


def pct(xs, p):
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def rng(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def series(runs):
    cpu = collections.OrderedDict()
    for run in runs:
        for x in run["arms"]:
            cpu.setdefault((x["shape"], x["path"]), collections.OrderedDict()).setdefault(
                x["bin"], []).append(x["server_cpu_us"])
    return cpu


def pair(by, x, y):
    return [p - q for p, q in zip(by[x], by[y])]


def verdict(d_aa):
    return max(abs(x) for x in d_aa), med([abs(x) for x in d_aa])


def t_guard(runs):
    cpu = series(runs)
    n = len(runs)
    print("**Throughput: statements/s derived from server CPU per statement (1e6 / median CPU µs), median over %d runs [min, max of the per-run rate]**\n" % n)
    print("| shape | path | A | A' (same bytes as A) | B (6b771ec1) | B2 (952c67e5) | B2 / A | B2 / B |")
    print("|---|---|---|---|---|---|---|---|")
    for (shape, path), by in cpu.items():
        def r(x):
            rates = [1e6 / v for v in by[x]]
            return "%s /s [%s, %s]" % (format(round(med(rates)), ","), format(round(min(rates)), ","), format(round(max(rates)), ","))
        print("| %s | %s | %s | %s | %s | %s | %.3f | %.3f |" % (
            shape, path, r("A"), r("A2"), r("B"), r("B2"),
            med(by["B2"]) / med(by["A"]), med(by["B2"]) / med(by["B"])))
    print("\n**Server CPU per statement (µs), median over %d runs [min, max]; the paired deltas are per run, then summarised the same way**\n" % n)
    print("| shape | path | A | A' | B | B2 | B - A | B2 - A | B2 - B | A - A' (S = max abs; typical = median abs) | B2 slower than A | B slower than A | B2 slower than B | A slower than A' |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for (shape, path), by in cpu.items():
        dba, d2a, d2b, daa = pair(by, "B", "A"), pair(by, "B2", "A"), pair(by, "B2", "B"), pair(by, "A", "A2")
        s, t = verdict(daa)
        f = lambda d: "%s (%+.1f %%)" % (rng(d, "%+.2f"), 100.0 * med(d) / med(by["A"]))
        gt = lambda d: "%d of %d" % (sum(1 for v in d if v > 0), len(d))
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s (S = %.2f, typical %.2f) | %s | %s | %s | %s |" % (
            shape, path, rng(by["A"], "%.2f"), rng(by["A2"], "%.2f"), rng(by["B"], "%.2f"), rng(by["B2"], "%.2f"),
            f(dba), f(d2a), f(d2b), rng(daa, "%+.2f"), s, t, gt(d2a), gt(dba), gt(d2b), gt(daa)))


def t_runs(runs):
    cpu = series(runs)
    for (shape, path), by in cpu.items():
        if shape != "update-range":
            continue
        print("\n`%s` / %s, server CPU per statement (µs) per run:\n" % (shape, path))
        print("| run | A | A' | B | B2 | A - A' | B - A | B2 - A | B2 - B |")
        print("|---|---|---|---|---|---|---|---|---|")
        for i in range(len(runs)):
            a, a2, b, b2 = by["A"][i], by["A2"][i], by["B"][i], by["B2"][i]
            print("| %d | %.1f | %.1f | %.1f | %.1f | %+.1f | %+.1f | %+.1f | %+.1f |" % (
                i, a, a2, b, b2, a - a2, b - a, b2 - a, b2 - b))


def t_pct(runs):
    print("| shape | path | arm | n (statements, all runs) | p0 | p25 | p50 | p90 | p99 | statements/s at p50 (derived) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    by = collections.OrderedDict()
    for run in runs:
        for x in run["arms"]:
            by.setdefault((x["shape"], x["path"], x["bin"]), []).append(x)
    for (shape, path, lab), xs in by.items():
        m = lambda k: med([x[k] for x in xs])
        print("| %s | %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %s /s |" % (
            shape, path, LABEL[lab], format(sum(x["ops"] for x in xs), ","),
            m("p0_us"), m("p25_us"), m("p50_us"), m("p90_us"), m("p99_us"), format(round(1e6 / m("p50_us")), ",")))


def t_waits(runs):
    print("| shape | path | arm | wall p50 per statement | server CPU | driver CPU | wall p50 - server CPU - driver CPU (socket, wake-up, scheduling) |")
    print("|---|---|---|---|---|---|---|")
    by = collections.OrderedDict()
    for run in runs:
        for x in run["arms"]:
            by.setdefault((x["shape"], x["path"], x["bin"]), []).append(x)
    for (shape, path, lab), xs in by.items():
        if lab not in ("A", "B", "B2"):
            continue
        w, s, d = med([x["p50_us"] for x in xs]), med([x["server_cpu_us"] for x in xs]), med([x["driver_cpu_us"] for x in xs])
        print("| %s | %s | %s | %.1f µs | %.1f µs (%.0f %%) | %.1f µs (%.0f %%) | %.1f µs (%.0f %%) |" % (
            shape, path, LABEL[lab], w, s, 100 * s / w, d, 100 * d / w, w - s - d, 100 * (w - s - d) / w))


def t_hosts(runs):
    print("| run | 1-min load at the run gate | gate wait | load before / after the run |")
    print("|---|---|---|---|")
    for run in runs:
        print("| %d | %.2f | %.0f s | %s / %s |" % (
            run["run"], run["quiet_gate"]["load"], run["quiet_gate"]["waited_s"],
            run["host_before"]["loadavg"].split()[0], run["host_after"]["loadavg"].split()[0]))
    cells = collections.OrderedDict()
    for run in runs:
        seen = set()
        for x in run["arms"]:
            h = x.get("host")
            key = (x["shape"], x["path"])
            if h is None or key in seen:
                continue
            seen.add(key)
            cells.setdefault("%s/%s" % key, []).append(h)
    print("\n| cell | 1-min load at the cell start, min - max | cell-runs above the load gate (2.0) | cell-runs clean | attempts, max | quiet wait before the cell, max | sibling CPUs busy %, median [max] | other sessions' share of server / driver CPU %, max | run-queue wait % of on-CPU time, median [max] | cell-runs with a competing build or test run |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    tot = collections.Counter()
    for cell, hs in cells.items():
        loads = [float(h["loadavg"].split()[0]) for h in hs]
        sib = [h.get("sibling_busy_pct") for h in hs if h.get("sibling_busy_pct") is not None]
        wt = [h.get("wait_pct") for h in hs if h.get("wait_pct") is not None]
        fs = [h.get("foreign_server_cpu_pct") for h in hs if h.get("foreign_server_cpu_pct") is not None]
        fd = [h.get("foreign_driver_cpu_pct") for h in hs if h.get("foreign_driver_cpu_pct") is not None]
        clean = sum(1 for h in hs if h.get("clean", False))
        tot["cells"] += len(hs)
        tot["above"] += sum(1 for x in loads if x > 2.0)
        tot["clean"] += clean
        tot["comp"] += sum(1 for h in hs if h["competing"])
        print("| %s | %.2f - %.2f | %d of %d | %d of %d | %d | %.0f s | %s | %s | %s | %d |" % (
            cell, min(loads), max(loads), sum(1 for x in loads if x > 2.0), len(loads), clean, len(hs),
            max(h.get("attempts", 1) for h in hs), max(h.get("quiet_waited_s", 0) for h in hs),
            ("%.1f [%.1f]" % (med(sib), max(sib))) if sib else "-",
            ("%.1f / %.1f" % (max(fs), max(fd))) if fs else "-",
            ("%.1f [%.1f]" % (med(wt), max(wt))) if wt else "-",
            sum(1 for h in hs if h["competing"])))
    print("\nall cell-runs: %d; above the load gate: %d; clean: %d; with a competing build or test run: %d" % (
        tot["cells"], tot["above"], tot["clean"], tot["comp"]))
    print("\nbad replies: %d" % sum(x["bad"] for run in runs for x in run["arms"] if "bad" in x))
    if "idle_cpu_us_per_s" in runs[0]:
        print("idle CPU per second per run and arm (µs): %s" % json.dumps(
            [{LABEL.get(k, k): round(v) for k, v in r["idle_cpu_us_per_s"].items()} for r in runs]))
    print("end state: %s" % json.dumps([r.get("end_state") for r in runs][0]))


if __name__ == "__main__":
    d = json.load(open(sys.argv[1]))
    runs = d["runs"]
    {"guard": t_guard, "runs": t_runs, "pct": t_pct, "hosts": t_hosts, "waits": t_waits}[sys.argv[sys.argv.index("--section") + 1]](runs)
