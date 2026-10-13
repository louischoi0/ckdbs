#!/usr/bin/env python3
"""BT-S5 tables for tools/bm_scan.py census output (arms A, A2 = A', B).

    bt_close_scan.py SECTION LABEL=FILE[.json|.json.gz] [LABEL=FILE ...]

SECTION: net | runs | pct | waits | identity | host | rps

Several files are pooled: a batch is one file (its own arm start order). The net row
is (scan server CPU - point server CPU on the same relation, arm, path, run) / rows,
paired by run; B - A and A' - A are per-run differences.  Only tables are printed.
"""
import collections, gzip, json, os, re, statistics, sys

CLEAN_ONLY = os.environ.get("CLEAN") == "1"   # drop (run, cell) windows the driver marked unclean

med = statistics.median
ARMS = ["A", "A2", "B"]
LAB = {"A": "A", "A2": "A'", "B": "B"}
PATHS = ("literal", "prepared")


def load(p):
    return json.load(gzip.open(p, "rt") if p.endswith(".gz") else open(p))


def pct(xs, q):
    xs = sorted(xs)
    k = (len(xs) - 1) * q / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def batches(args):
    return [(a.split("=", 1)[0], load(a.split("=", 1)[1])) for a in args]


def scan_cells(bs):
    """(cell) in order of first appearance."""
    seen = []
    for _, d in bs:
        for r in d["runs"]:
            for x in r["arms"]:
                if x["kind"] == "scan" and x["cell"] not in seen:
                    seen.append(x["cell"])
    return seen


def nets(bs, cell, path):
    """-> list of (batch, run, {arm: net ns/row}), plus rel, rows."""
    out = []
    rel = rows = None
    for lab, d in bs:
        for r in d["runs"]:
            pt, sc = {}, {}
            for x in r["arms"]:
                if x["path"] != path:
                    continue
                if x["kind"] == "scan" and x["cell"] == cell:
                    sc[x["arm"]] = x
                    rel, rows = x["rel"], x["rows"]
            for x in r["arms"]:
                if x["kind"] == "point" and x["path"] == path and x["rel"] == rel:
                    pt[x["arm"]] = x
            if not sc:
                continue
            clean = all(x["host"].get("clean", False) for x in list(sc.values()) + list(pt.values()))
            if CLEAN_ONLY and not clean:
                continue
            out.append((lab, r["run"], {a: (sc[a]["server_cpu_us"] - pt[a]["server_cpu_us"]) * 1e3 / sc[a]["rows"] for a in sc}))
    return out, rel, rows


def rng(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def t_net(bs):
    for path in PATHS:
        print("\n**%s path**\n" % path)
        print("| cell | rows | n runs | A | A' | B | rows/s A (derived) | rows/s B (derived) | B - A, paired by run, median [min, max] | B - A as % of A | B below A | A' - A: median [min, max] | typical \\|A' - A\\| | largest \\|A' - A\\| | B - A' |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for cell in scan_cells(bs):
            ns, rel, rows = nets(bs, cell, path)
            A = [v["A"] for _, _, v in ns]
            A2 = [v["A2"] for _, _, v in ns]
            B = [v["B"] for _, _, v in ns]
            dBA = [b - a for a, b in zip(A, B)]
            dAA = [b - a for a, b in zip(A, A2)]
            dBA2 = [b - a for a, b in zip(A2, B)]
            print("| %s | %s | %d | %s ns | %s ns | %s ns | %s /s | %s /s | %s ns | %+.1f %% | %d of %d | %s ns | %.2f ns | %.2f ns | %+.1f ns |" % (
                cell, format(rows, ","), len(A), rng(A), rng(A2), rng(B),
                format(round(1e9 / med(A)), ","), format(round(1e9 / med(B)), ","),
                rng(dBA, "%+.1f"), 100 * med(dBA) / med(A), sum(1 for v in dBA if v < 0), len(dBA),
                rng(dAA, "%+.1f"), med([abs(v) for v in dAA]), max(abs(v) for v in dAA), med(dBA2)))


def t_groups(bs):
    """One row per (group, cell, path); a group is every batch sharing a label."""
    labs = []
    for l, _ in bs:
        if l not in labs:
            labs.append(l)
    print("| group | cell | path | n runs | A | A' | B | B - A, paired, median [min, max] | B - A as % of A | B below A | A' - A typical / largest \\|gap\\| |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for cell in scan_cells(bs):
        for path in PATHS:
            for l in labs + ([] if os.environ.get("NOPOOL") == "1" else ["pooled"]):
                sub = bs if l == "pooled" else [b for b in bs if b[0] == l]
                ns, rel, rows = nets(sub, cell, path)
                if not ns:
                    continue
                A = [v["A"] for _, _, v in ns]
                A2 = [v["A2"] for _, _, v in ns]
                B = [v["B"] for _, _, v in ns]
                dBA = [b - a for a, b in zip(A, B)]
                dAA = [b - a for a, b in zip(A, A2)]
                print("| %s | %s | %s | %d | %.1f ns | %.1f ns | %.1f ns | %s ns | %+.1f %% | %d of %d | %.2f / %.2f ns |" % (
                    l, cell, path, len(A), med(A), med(A2), med(B), rng(dBA, "%+.1f"), 100 * med(dBA) / med(A),
                    sum(1 for v in dBA if v < 0), len(dBA), med([abs(v) for v in dAA]), max(abs(v) for v in dAA)))


def t_runs(bs):
    for path in PATHS:
        for cell in scan_cells(bs):
            ns, rel, rows = nets(bs, cell, path)
            print("\n`%s` / %s, net row per run (ns):\n" % (cell, path))
            print("| batch | run | A | A' | B | B - A | A' - A |")
            print("|---|---|---|---|---|---|---|")
            for lab, run, v in ns:
                print("| %s | %d | %.1f ns | %.1f ns | %.1f ns | %+.1f ns | %+.1f ns |" % (
                    lab, run, v["A"], v["A2"], v["B"], v["B"] - v["A"], v["A2"] - v["A"]))


def t_pct(bs):
    print("| cell | path | arm | n (scans, all runs) | p0 | p25 | p50 | p90 | p95 | p99 | scans/s at p50 (derived) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for cell in scan_cells(bs):
        for path in PATHS:
            for arm in ARMS:
                lat = []
                for _, d in bs:
                    for r in d["runs"]:
                        for x in r["arms"]:
                            if x["kind"] == "scan" and x["cell"] == cell and x["path"] == path and x["arm"] == arm:
                                if CLEAN_ONLY and not x["host"].get("clean", False):
                                    continue
                                lat += x["lat_us"]
                p50 = pct(lat, 50)
                print("| %s | %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %s /s |" % (
                    cell, path, LAB[arm], format(len(lat), ","), min(lat), pct(lat, 25), p50, pct(lat, 90),
                    pct(lat, 95), pct(lat, 99), format(round(1e6 / p50), ",")))


def t_waits(bs):
    print("| cell | path | arm | wall mean | per-row work (rows x net row) | fixed statement (point SELECT server CPU) | client + socket (wall mean - scan server CPU) | per-row share of wall | driver CPU |")
    print("|---|---|---|---|---|---|---|---|---|")
    for cell in scan_cells(bs):
        for path in PATHS:
            ns, rel, rows = nets(bs, cell, path)
            sc = collections.defaultdict(list)
            pt = collections.defaultdict(list)
            for _, d in bs:
                for r in d["runs"]:
                    for x in r["arms"]:
                        if x["path"] != path:
                            continue
                        if x["kind"] == "scan" and x["cell"] == cell:
                            sc[x["arm"]].append(x)
                        if x["kind"] == "point" and x["rel"] == rel:
                            pt[x["arm"]].append(x)
            for arm in ARMS:
                wall = med([c["p_mean"] for c in sc[arm]])
                scpu = med([c["server_cpu_us"] for c in sc[arm]])
                pcpu = med([c["server_cpu_us"] for c in pt[arm]])
                perrow = med([v[arm] for _, _, v in ns]) * rows / 1e3
                dcpu = med([c["driver_cpu_us"] for c in sc[arm]])
                print("| %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.0f %% | %.1f µs |" % (
                    cell, path, LAB[arm], wall, perrow, pcpu, wall - scpu, 100 * perrow / wall, dcpu))
    print("\nwait counters, summed over every run and batch (scan cells; then point cells):\n")
    print("| kind | cell | path | arm | lookups | lookups_instance | window_wait_us | pool_misses | bad |")
    print("|---|---|---|---|---|---|---|---|---|")
    keys = collections.OrderedDict()
    for _, d in bs:
        for r in d["runs"]:
            for x in r["arms"]:
                k = (x["kind"], x["cell"], x["path"], x["arm"])
                s = keys.setdefault(k, collections.Counter())
                for f in ("lookups", "lookups_instance", "window_wait_us", "pool_misses", "bad"):
                    s[f] += x[f] or 0
    for (kind, cell, path, arm), s in sorted(keys.items(), key=lambda kv: (kv[0][0] != "scan", kv[0][1], kv[0][2], ARMS.index(kv[0][3]))):
        print("| %s | %s | %s | %s | %d | %d | %d | %d | %d |" % (kind, cell, path, LAB[arm], s["lookups"], s["lookups_instance"], s["window_wait_us"], s["pool_misses"], s["bad"]))


def t_identity(bs):
    for lab, d in bs:
        r0 = d["runs"][0]
        print("\nbatch %s, run 0: digest %s" % (lab, json.dumps(r0["digest"])))
        for cell, per in r0["analyze"].items():
            for arm in ARMS:
                v = per[arm]
                m = re.search(r"examined=\d+ pages=\d+", v)
                st = re.search(r"step 0 .*", v)
                print("  %s %s: %s | %s" % (cell, arm, m.group(0) if m else "?", st.group(0) if st else v[:200]))
    print("\nrows per scan cell:", sorted({(x["cell"], x["rows"]) for _, d in bs for r in d["runs"] for x in r["arms"] if x["kind"] == "scan"}))
    print("bad replies (all cells, all runs):", sum(x["bad"] for _, d in bs for r in d["runs"] for x in r["arms"]))


def t_host(bs):
    for lab, d in bs:
        print("\nbatch %s: arm order per run: %s" % (lab, [r["arm_order"] for r in d["runs"]][:2]))
        print("| run | gate wait | load at gate release | load at first cell | cells | clean | attempts max | quiet wait before cell, max | load at cells min-max | sibling busy max | foreign srv/drv max | competing | idle CPU/s A, A', B (µs) |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for r in d["runs"]:
            hs = [x["host"] for x in r["arms"] if "host" in x]
            uniq = {}
            for x in r["arms"]:
                uniq.setdefault((x["cell"], x["path"]), x["host"])
            hs = list(uniq.values())
            loads = [float(h["loadavg"].split()[0]) for h in hs]
            print("| %d | %.0f s | %.2f | %.2f | %d | %d of %d | %d | %.0f s | %.2f-%.2f | %.1f %% | %.1f / %.1f %% | %s | %s |" % (
                r["run"], r["quiet_gate"]["waited_s"], r["quiet_gate"]["load"], loads[0], len(hs),
                sum(1 for h in hs if h.get("clean")), len(hs), max(h.get("attempts", 1) for h in hs),
                max(h.get("quiet_waited_s", 0) for h in hs), min(loads), max(loads),
                max(h.get("sibling_busy_pct", 0) for h in hs), max(h.get("foreign_server_cpu_pct", 0) for h in hs),
                max(h.get("foreign_driver_cpu_pct", 0) for h in hs), any(h["competing"] for h in hs),
                "/".join("%.0f" % r["idle_cpu_us_per_s"][a] for a in ARMS)))


if __name__ == "__main__":
    sec = sys.argv[1]
    bs = batches(sys.argv[2:])
    {"net": t_net, "groups": t_groups, "runs": t_runs, "pct": t_pct, "waits": t_waits, "identity": t_identity, "host": t_host}[sec](bs)
