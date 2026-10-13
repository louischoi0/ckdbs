#!/usr/bin/env python3
"""BT-S5 tables for tools/bn_census.py output (arms A, A2 = A', B), shapes select-pk and update-range.

    bt_close_bn.py SECTION LABEL=FILE[.json|.json.gz] [LABEL=FILE ...]

SECTION: guard | runs | pct | waits | host | end

Files are pooled as batches (each batch has its own server start order); deltas are paired
by run within a batch.  S = largest |A - A'|, typical = median |A - A'|.
"""
import collections, gzip, json, os, statistics, sys

med = statistics.median
ARMS = ["A", "A2", "B"]
LAB = {"A": "A", "A2": "A'", "B": "B"}


def load(p):
    return json.load(gzip.open(p, "rt") if p.endswith(".gz") else open(p))


def batches(args):
    return [(a.split("=", 1)[0], load(a.split("=", 1)[1])) for a in args]


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def rng(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def series(bs):
    """(shape, path) -> arm -> [server_cpu_us per run, in batch/run order]"""
    cpu = collections.OrderedDict()
    for _, d in bs:
        for r in d["runs"]:
            unclean = {(x["shape"], x["path"]) for x in r["arms"] if not x["host"].get("clean", False)}
            for x in r["arms"]:
                if os.environ.get("CLEAN") == "1" and (x["shape"], x["path"]) in unclean:
                    continue
                cpu.setdefault((x["shape"], x["path"]), collections.OrderedDict()).setdefault(x["bin"], []).append(x["server_cpu_us"])
    return cpu


def t_guard(bs):
    cpu = series(bs)
    n = sum(len(d["runs"]) for _, d in bs)
    print("**Throughput, derived (1e6 / median server CPU per statement), %d paired runs**\n" % n)
    print("| shape | path | A | A' | B | B / A (CPU per statement) |")
    print("|---|---|---|---|---|---|")
    for (shape, path), by in cpu.items():
        print("| %s | %s | %s /s | %s /s | %s /s | %.4f |" % (
            shape, path, format(round(1e6 / med(by["A"])), ","), format(round(1e6 / med(by["A2"])), ","),
            format(round(1e6 / med(by["B"])), ","), med(by["B"]) / med(by["A"])))
    print("\n**Server CPU per statement (µs), median over %d runs [min, max]; deltas paired by run**\n" % n)
    print("| shape | path | A | A' | B | B - A | B - A' | A' - A | typical \\|A' - A\\| | largest \\|A' - A\\| | B slower than A | A' slower than A |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for (shape, path), by in cpu.items():
        dBA = [b - a for a, b in zip(by["A"], by["B"])]
        dBA2 = [b - a for a, b in zip(by["A2"], by["B"])]
        dAA = [b - a for a, b in zip(by["A"], by["A2"])]
        prec = "%.2f" if shape == "select-pk" else "%.1f"
        sg = "%+.2f" if shape == "select-pk" else "%+.1f"
        print("| %s | %s | %s µs | %s µs | %s µs | %s µs (%+.2f %%) | %s µs | %s µs | %.2f µs | %.2f µs | %d of %d | %d of %d |" % (
            shape, path, rng(by["A"], prec), rng(by["A2"], prec), rng(by["B"], prec), rng(dBA, sg), 100 * med(dBA) / med(by["A"]),
            rng(dBA2, sg), rng(dAA, sg), med([abs(v) for v in dAA]), max(abs(v) for v in dAA),
            sum(1 for v in dBA if v > 0), len(dBA), sum(1 for v in dAA if v > 0), len(dAA)))
    # per batch
    if len(bs) > 1:
        print("\n**By batch (server start order differs), median µs**\n")
        print("| batch | start order | shape | path | A | A' | B | B - A | A' - A |")
        print("|---|---|---|---|---|---|---|---|---|")
        for lab, d in bs:
            c = series([(lab, d)])
            order = d["args"].get("arm") or []
            order = ",".join(a.split("=")[0] for a in order)
            for (shape, path), by in c.items():
                dBA = [b - a for a, b in zip(by["A"], by["B"])]
                dAA = [b - a for a, b in zip(by["A"], by["A2"])]
                print("| %s | %s | %s | %s | %.2f | %.2f | %.2f | %+.2f | %+.2f |" % (
                    lab, order, shape, path, med(by["A"]), med(by["A2"]), med(by["B"]), med(dBA), med(dAA)))


def t_groups(bs):
    labs = []
    for l, _ in bs:
        if l not in labs:
            labs.append(l)
    print("| group | shape | path | n runs | A | A' | B | B - A, paired, median [min, max] | B - A as % of A | B slower than A | A' - A typical / largest \\|gap\\| |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    keys = list(series(bs).keys())
    for (shape, path) in keys:
        for l in labs + ([] if os.environ.get("NOPOOL") == "1" else ["pooled"]):
            sub = bs if l == "pooled" else [b for b in bs if b[0] == l]
            by = series(sub)[(shape, path)]
            dBA = [b - a for a, b in zip(by["A"], by["B"])]
            dAA = [b - a for a, b in zip(by["A"], by["A2"])]
            prec = "%.2f" if shape == "select-pk" else "%.1f"
            sg = "%+.2f" if shape == "select-pk" else "%+.1f"
            print("| %s | %s | %s | %d | %s µs | %s µs | %s µs | %s µs | %+.2f %% | %d of %d | %.2f / %.2f µs |" % (
                l, shape, path, len(dBA), prec % med(by["A"]), prec % med(by["A2"]), prec % med(by["B"]), rng(dBA, sg),
                100 * med(dBA) / med(by["A"]), sum(1 for v in dBA if v > 0), len(dBA),
                med([abs(v) for v in dAA]), max(abs(v) for v in dAA)))


def t_runs(bs):
    for (shape, path), by in series(bs).items():
        if shape != "update-range":
            continue
        print("\n`%s` / %s, server CPU per statement per run (µs):\n" % (shape, path))
        print("| batch | run | A | A' | B | B - A | A' - A |")
        print("|---|---|---|---|---|---|---|")
        i = 0
        for lab, d in bs:
            for r in d["runs"]:
                a, a2, b = by["A"][i], by["A2"][i], by["B"][i]
                print("| %s | %d | %.1f | %.1f | %.1f | %+.1f | %+.1f |" % (lab, r["run"], a, a2, b, b - a, a2 - a))
                i += 1


def grouped(bs):
    by = collections.OrderedDict()
    for _, d in bs:
        for run in d["runs"]:
            for x in run["arms"]:
                by.setdefault((x["shape"], x["path"], x["bin"]), []).append(x)
    return by


def t_pct(bs):
    print("| shape | path | arm | n (statements, all runs) | p0 | p25 | p50 | p90 | p99 | statements/s at p50 (derived) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for (shape, path, lab), xs in grouped(bs).items():
        m = lambda k: med([x[k] for x in xs])
        print("| %s | %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %s /s |" % (
            shape, path, LAB[lab], format(sum(x["ops"] for x in xs), ","),
            m("p0_us"), m("p25_us"), m("p50_us"), m("p90_us"), m("p99_us"), format(round(1e6 / m("p50_us")), ",")))
    print("\n(percentiles are the median over runs of each run's percentile; n is the statements pooled)")


def t_waits(bs):
    print("| shape | path | arm | wall p50 per statement | server CPU | driver CPU | wall p50 - server CPU - driver CPU (socket, wake-up, scheduling) |")
    print("|---|---|---|---|---|---|---|")
    for (shape, path, lab), xs in grouped(bs).items():
        w, s, d = med([x["p50_us"] for x in xs]), med([x["server_cpu_us"] for x in xs]), med([x["driver_cpu_us"] for x in xs])
        print("| %s | %s | %s | %.1f µs | %.1f µs (%.0f %%) | %.1f µs (%.0f %%) | %.1f µs (%.0f %%) |" % (
            shape, path, LAB[lab], w, s, 100 * s / w, d, 100 * d / w, w - s - d, 100 * (w - s - d) / w))


def t_host(bs):
    for lab, d in bs:
        runs = d["runs"]
        print("\nbatch %s (start order %s):\n" % (lab, ",".join(a.split("=")[0] for a in d["args"]["arm"])))
        print("| run | gate wait | load at gate release | load before / after the run | cells | clean | attempts max | quiet wait max | load at cells | sibling busy max | foreign srv / drv max | wait % of on-CPU, median [max] | competing |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for r in runs:
            uniq = {}
            for x in r["arms"]:
                uniq.setdefault((x["shape"], x["path"]), x["host"])
            hs = list(uniq.values())
            loads = [float(h["loadavg"].split()[0]) for h in hs]
            wt = [h["wait_pct"] for h in hs if h.get("wait_pct") is not None]
            print("| %d | %.0f s | %.2f | %s / %s | %d | %d of %d | %d | %.0f s | %.2f-%.2f | %.1f %% | %.1f / %.1f %% | %.1f [%.1f] | %s |" % (
                r["run"], r["quiet_gate"]["waited_s"], r["quiet_gate"]["load"], r["host_before"]["loadavg"].split()[0],
                r["host_after"]["loadavg"].split()[0], len(hs), sum(1 for h in hs if h.get("clean")), len(hs),
                max(h.get("attempts", 1) for h in hs), max(h.get("quiet_waited_s", 0) for h in hs), min(loads), max(loads),
                max(h.get("sibling_busy_pct", 0) for h in hs), max(h.get("foreign_server_cpu_pct", 0) for h in hs),
                max(h.get("foreign_driver_cpu_pct", 0) for h in hs), med(wt), max(wt), any(h["competing"] for h in hs)))
    print("\nbad replies: %d" % sum(x["bad"] for _, d in bs for r in d["runs"] for x in r["arms"]))


def t_end(bs):
    ok = collections.Counter()
    for _, d in bs:
        for r in d["runs"]:
            for arm, st in r["end_state"].items():
                for k, v in st.items():
                    ok[(arm, k, bool(v))] += 1
    print("end-state checks (arm, check, value): count")
    for k, v in sorted(ok.items()):
        print(" ", k, v)
    for _, d in bs:
        for r in d["runs"]:
            print("run", r["run"], "pages-unspilled", json.dumps(r.get("pages-unspilled")), "pages-spilled", json.dumps(r.get("pages-spilled")))
            break


if __name__ == "__main__":
    sec = sys.argv[1]
    bs = batches(sys.argv[2:])
    {"guard": t_guard, "groups": t_groups, "runs": t_runs, "pct": t_pct, "waits": t_waits, "host": t_host, "end": t_end}[sec](bs)
