#!/usr/bin/env python3
"""Tables for BM-S6's close (`workorder-bm-scan-row-kernel.md` §5.2, BM-Q12).

    bm_close_table.py census CENSUS.json [--pg PG.json] [--pg2 PG2.json] --section SEC
    bm_close_table.py bn     BN.json[,BN2.json,...] --section SEC

`census` reads a `tools/bm_scan.py census --arm ...` file, `bn` a
`tools/bn_census.py --arm ...` file (several: one per row count, in the
order given). The arms are A (BM's opening commit), A2 (the same commit from a
second export, printed A'), B (BM's closing commit) and Boff (B with
`scan_kernel = off`); the table of an arm absent from the file is skipped. A
fifth, Apad (printed A-al64), is A's source built with `-falign-functions=64`:
**a layout probe, never part of the bar**. (A rebuild of A from a second export
is byte-identical to A, so A against A' bounds the harness's noise and not the
code layout; the probe shows how far a layout change alone moves a cell.)

**The bar (BM-Q12), fixed before the data were read.** For one cell, per run
`r`, `d(B-A)_r = B_r - A_r` and `d(A-A')_r = A_r - A2_r`, the same quantity on
the same statements in the same run. The spread `S` is the largest
`|A_r - A2_r|` over the runs: the A-against-A' difference the identical code
shows through code layout and the host. The cell is **within the spread** when
`|median_r d(B-A)_r| <= S`, **slower** when the median exceeds `+S` and
**faster** when it falls under `-S`. Only the 100 % output scan, the point
`SELECT` and the every-row `UPDATE` are bars; every other cell carries the same
reading as information.

Sections (census): net, delta, pg, rate, guard, pct, point, hosts, digest, lookups.
Sections (bn): guard, pct, hosts.
"""
import argparse, collections, json, os, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bm_scan_table import jload, pct  # noqa: E402

PCTS = (("p0", 0), ("p25", 25), ("p50", 50), ("p90", 90), ("p95", 95), ("p99", 99))
LABEL = {"A": "A", "A2": "A'", "B": "B", "Boff": "B-off", "Apad": "A-al64"}
ORDER = ("A", "A2", "B", "Boff", "Apad")
GATE = 2.0
med = statistics.median


def present(runs):
    seen = []
    for run in runs:
        for arm in run["arms"]:
            lab = arm.get("arm") or arm.get("bin")
            if lab not in seen:
                seen.append(lab)
    return [x for x in ORDER if x in seen]


def label_of(arm):
    return arm.get("arm") or arm.get("bin")


def rng(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def verdict(d_ba, d_aa):
    """(S, word) for paired per-run deltas B-A and A-A'. S is the bar (the
    largest A-against-A' gap in any run); the word also says where the median
    B-A stands against the *typical* gap (the median over runs of |A - A'|),
    because one outlying run sets S and a consistent small delta can hide
    inside it."""
    s = max(abs(x) for x in d_aa)
    t = med([abs(x) for x in d_aa])
    m = med(d_ba)
    if m > s:
        return s, "slower than the spread"
    if m < -s:
        return s, "faster than the spread"
    if m > t:
        return s, "within the spread (B - A above the typical gap %.3g)" % t
    if m < -t:
        return s, "within the spread (B - A below minus the typical gap %.3g)" % t
    return s, "within the spread (inside the typical gap %.3g)" % t


# -- bm_scan census ----------------------------------------------------------------

def census_nets(runs, path):
    """{arm: {cell: [net row ns per run]}}, net = (scan - point on the same
    relation, arm, path and run) / rows."""
    out = collections.defaultdict(lambda: collections.OrderedDict())
    for run in runs:
        point = {}
        for x in run["arms"]:
            if x["kind"] == "point" and x["path"] == path:
                point[(x["arm"], x["rel"])] = x["server_cpu_us"]
        for x in run["arms"]:
            if x["kind"] == "scan" and x["path"] == path:
                out[x["arm"]].setdefault(x["cell"], []).append(
                    (x["server_cpu_us"] - point[(x["arm"], x["rel"])]) * 1e3 / x["rows"])
    return out


def scan_stat(runs, path, key):
    """{arm: {cell: [key per run]}} of the scan arms."""
    out = collections.defaultdict(lambda: collections.OrderedDict())
    for run in runs:
        for x in run["arms"]:
            if x["kind"] == "scan" and x["path"] == path:
                out[x["arm"]].setdefault(x["cell"], []).append(x[key])
    return out


def cell_info(runs):
    d = collections.OrderedDict()
    for x in runs[0]["arms"]:
        if x["kind"] == "scan":
            d.setdefault(x["cell"], x)
    return d


def pg_nets(path):
    """Net rows of `bn_pg_scan.py --shapes bm` output; `path` may be several
    files, comma-separated: their runs are pooled."""
    runs = [r for p in path.split(",") for r in jload(p)["runs"]]
    pg = {"runs": runs}
    out = {}
    for run in pg["runs"]:
        pts = {x["rel"]: x["server_cpu_us"] for x in run["arms"] if x["kind"] == "point"}
        for x in run["arms"]:
            if x["kind"] == "scan":
                out.setdefault(x["cell"], []).append(
                    (x["server_cpu_us"] - pts[x["rel"]]) * 1e3 / x["rows"])
    return out


def base_nets(path):
    """{cell: [net row ns per run]} of a bm_scan census file with no arm field
    (BM-S1's), literal path."""
    runs = jload(path)["runs"]
    out = {}
    for run in runs:
        point = {}
        for x in run["arms"]:
            if x["kind"] == "point" and x["path"] == "literal":
                point[x["rel"]] = x["server_cpu_us"]
        for x in run["arms"]:
            if x["kind"] == "scan" and x["path"] == "literal":
                out.setdefault(x["cell"], []).append((x["server_cpu_us"] - point[x["rel"]]) * 1e3 / x["rows"])
    return out


def t_net(a, runs):
    arms = present(runs)
    info = cell_info(runs)
    base = base_nets(a.base) if a.base else {}
    for path in ("literal", "prepared"):
        nets = census_nets(runs, path)
        print("\n**%s path: net row, ns (median over %d runs [min, max]); the net row is (scan CPU - point-select CPU "
              "on the same relation, arm, path and run) / rows scanned**\n" % (path, len(runs)))
        cols = ["cell", "rows scanned"] + [LABEL[x] for x in arms]
        if base and path == "literal":
            cols += ["BM-S1 file, f5a7efb2 (literal)"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for cell, first in info.items():
            n = {x: nets[x][cell] for x in arms if cell in nets[x]}
            row = [cell, format(first["rows"], ",")] + ["%s ns" % rng(n[x]) if x in n else "-" for x in arms]
            if base and path == "literal":
                row += ["%s ns" % rng(base[cell]) if cell in base else "not in that file"]
            print("| " + " | ".join(row) + " |")


def t_delta(a, runs):
    arms = present(runs)
    info = cell_info(runs)
    for path in ("literal", "prepared"):
        nets = census_nets(runs, path)
        print("\n**%s path: paired deltas of the net row, ns (median over %d runs [min, max]; per-run pairs); "
              "S is the largest |A - A'| in any run (BM-Q12's spread); only `sel-100` is a bar here**\n"
              % (path, len(runs)))
        cols = ["cell", "B - A", "B-off - A (S2 alone)", "B - B-off (S3, S4)", "A - A' (S)", "B against S",
                "B > A in k of %d runs" % len(runs)]
        if "Apad" in arms:
            cols += ["A-al64 - A (layout probe)"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for cell in info:
            n = {x: nets[x][cell] for x in arms if cell in nets[x]}
            pair = lambda x, y: [p - q for p, q in zip(n[x], n[y])] if x in n and y in n else None
            dba, doa, dbo, daa = pair("B", "A"), pair("Boff", "A"), pair("B", "Boff"), pair("A", "A2")
            f = lambda d: ("%s (%+.1f %%)" % (rng(d, "%+.2f"), 100.0 * med(d) / med(n["A"]))) if d else "-"
            s, word = verdict(dba, daa)
            row = [cell, f(dba), f(doa), f(dbo), "%s (S = %.2f)" % (rng(daa, "%+.2f"), s), word,
                   "%d" % sum(1 for x in dba if x > 0)]
            if "Apad" in arms:
                row += [f(pair("Apad", "A"))]
            print("| " + " | ".join(row) + " |")


def t_pg(a, runs):
    arms = present(runs)
    info = cell_info(runs)
    pg1 = pg_nets(a.pg) if a.pg else {}
    pg2 = pg_nets(a.pg2) if a.pg2 else {}
    nets = census_nets(runs, "literal")
    print("\n**Against PostgreSQL 18.6 on the same host and pinning (simple query protocol, a literal per "
          "statement; KDS literal path); a floor, not a reference**\n")
    cols = ["cell", "rows scanned", "A", "B", "PG18 vacuumed", "A / PG", "B / PG", "PG18 not vacuumed",
            "A / PG", "B / PG"]
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    for cell, first in info.items():
        row = [cell, format(first["rows"], ",")]
        for x in ("A", "B"):
            pass
        ma, mb = med(nets["A"][cell]), med(nets["B"][cell])
        row += ["%.1f ns" % ma, "%.1f ns" % mb]
        for pg in (pg1, pg2):
            if cell in pg and med(pg[cell]) <= 0:
                # PostgreSQL's primary-key lookup costs more than this scan, so (scan - point) is not a row cost.
                row += ["%s ns (negative: not a row cost)" % rng(pg[cell]), "n/m", "n/m"]
            elif cell in pg:
                m = med(pg[cell])
                row += ["%s ns" % rng(pg[cell]), "%.2f" % (ma / m), "%.2f" % (mb / m)]
            else:
                row += ["not measured", "-", "-"] if (pg is pg1) else ["not measured", "-", "-"]
        print("| " + " | ".join(row) + " |")


def t_rate(a, runs):
    arms = present(runs)
    info = cell_info(runs)
    for path in ("literal", "prepared"):
        nets = census_nets(runs, path)
        print("\n**%s path, scan rate: rows per second derived from the median net row (1e9 / ns)**\n" % path)
        cols = ["cell", "rows scanned"] + ["%s rows/s" % LABEL[x] for x in arms] + [
            "B / A", "B-off / A"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for cell, first in info.items():
            m = {x: med(nets[x][cell]) for x in arms if cell in nets[x]}
            rate = lambda x: 1e9 / m[x]
            row = [cell, format(first["rows"], ",")] + ["%.2f M" % (rate(x) / 1e6) if x in m else "-"
                                                          for x in arms]
            row += ["%.3f" % (rate("B") / rate("A")) if "A" in m and "B" in m else "-",
                    "%.3f" % (rate("Boff") / rate("A")) if "A" in m and "Boff" in m else "-"]
            print("| " + " | ".join(row) + " |")


def stmt_table(title, series, arms, per_unit="statement"):
    """Statement-level server CPU (us) by arm, with statements/s derived and the
    paired deltas and the bar's reading. `series` = {arm: [cpu us per run]}."""
    cols = ["cell"] + ["%s server CPU" % LABEL[x] for x in arms] + [
        "%s statements/s (CPU-derived)" % LABEL[x] for x in ("A", "B") if x in arms] + [
        "B - A", "B-off - A", "A - A' (S = max abs)", "B against S"]
    print(title)
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    return cols


def t_guard_census(a, runs):
    arms = present(runs)
    info = cell_info(runs)
    for path in ("literal", "prepared"):
        cpu = scan_stat(runs, path, "server_cpu_us")
        p50 = scan_stat(runs, path, "p_p50")
        print("\n**Guard: the 100 %% output scan (`sel-100`, and the same shape at 200 and 1,000 rows), %s path. "
              "Metric: the statement's server CPU; p50 wall beside it**\n" % path)
        stmt_table("", cpu, arms)
        for cell in ("out-200", "out-1k", "sel-100"):
            if cell not in info:
                continue
            c = {x: cpu[x][cell] for x in arms}
            row = [cell] + ["%s µs" % rng(c[x]) for x in arms]
            row += ["%s /s" % format(round(1e6 / med(c[x])), ",") for x in ("A", "B") if x in arms]
            pair = lambda x, y: [p - q for p, q in zip(c[x], c[y])] if x in c and y in c else None
            dba, doa, daa = pair("B", "A"), pair("Boff", "A"), pair("A", "A2")
            f = lambda d: ("%s µs (%+.1f %%)" % (rng(d, "%+.1f"), 100.0 * med(d) / med(c["A"]))) if d else "-"
            row += [f(dba), f(doa)]
            s, word = verdict(dba, daa)
            row += ["%s µs (S = %.1f µs)" % (rng(daa, "%+.1f"), s), word]
            print("| " + " | ".join(row) + " |")
        print("\nwall p50 per statement (median over runs of each run's p50), statements/s derived:\n")
        print("| cell | " + " | ".join("%s wall p50" % LABEL[x] for x in arms) + " |")
        print("|---|" + "---|" * len(arms))
        for cell in ("out-200", "out-1k", "sel-100"):
            if cell in info:
                print("| %s | %s |" % (cell, " | ".join(
                    "%.1f µs (%s /s)" % (med(p50[x][cell]), format(round(1e6 / med(p50[x][cell])), ","))
                    for x in arms)))
        # B > A count per cell
        print()
        for cell in ("out-200", "out-1k", "sel-100"):
            if cell in info and "A" in cpu and "B" in cpu:
                gt = sum(1 for p, q in zip(cpu["B"][cell], cpu["A"][cell]) if p > q)
                print("- `%s`: B slower than A in %d of %d runs" % (cell, gt, len(cpu["A"][cell])))


def t_point_census(a, runs):
    arms = present(runs)
    for path in ("literal", "prepared"):
        pts = collections.defaultdict(lambda: collections.OrderedDict())
        for run in runs:
            for x in run["arms"]:
                if x["kind"] == "point" and x["path"] == path:
                    pts[x["arm"]].setdefault(x["rel"], []).append(x["server_cpu_us"])
        print("\n**Point `SELECT * FROM <rel> WHERE id = ?` (the pk lookup; it does not scan), %s path, "
              "server CPU per statement over %d runs of 3,000 statements**\n" % (path, len(runs)))
        cols = ["relation (rows)"] + ["%s" % LABEL[x] for x in arms] + [
            "A statements/s (CPU-derived)", "B statements/s (CPU-derived)", "B - A", "B-off - A",
            "A - A' (S = max abs)", "B against S"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        rows = {"s200": 200, "s1k": 1000, "s": 10000, "s100k": 100000, "wd": 10000, "nul0": 10000,
                "nul50": 10000, "srec": 10000}
        for rel, c in pts["A"].items():
            c = {x: pts[x][rel] for x in arms}
            row = ["%s (%s)" % (rel, format(rows[rel], ","))] + ["%s µs" % rng(c[x], "%.2f") for x in arms]
            row += ["%s /s" % format(round(1e6 / med(c["A"])), ","), "%s /s" % format(round(1e6 / med(c["B"])), ",")]
            pair = lambda x, y: [p - q for p, q in zip(c[x], c[y])]
            dba, doa, daa = pair("B", "A"), pair("Boff", "A"), pair("A", "A2")
            f = lambda d: "%s µs (%+.1f %%)" % (rng(d, "%+.2f"), 100.0 * med(d) / med(c["A"]))
            s, word = verdict(dba, daa)
            row += [f(dba), f(doa), "%s µs (S = %.2f µs)" % (rng(daa, "%+.2f"), s), word]
            print("| " + " | ".join(row) + " |")


def lat_rows(runs, path_filter=None):
    """{(cell/kind label, path): {arm: pooled lat list}} in first-seen order."""
    d = collections.OrderedDict()
    for run in runs:
        for x in run["arms"]:
            key = (x["cell"], x["path"])
            d.setdefault(key, collections.OrderedDict()).setdefault(x["arm"], []).extend(x["lat_us"])
    return d


def t_pct(a, runs):
    arms = present(runs)
    print("| cell | path | arm | n | " + " | ".join(n for n, _ in PCTS) + " | statements/s at p50 (derived) |")
    print("|---|---|---|---|" + "---|" * (len(PCTS) + 1))
    for (cell, path), by in lat_rows(runs).items():
        for x in arms:
            if x not in by:
                continue
            lat = by[x]
            print("| %s | %s | %s | %s | %s | %s /s |" % (
                cell, path, LABEL[x], format(len(lat), ","),
                " | ".join("%.1f µs" % pct(lat, p) if p else "%.1f µs" % min(lat) for _, p in PCTS),
                format(round(1e6 / pct(lat, 50)), ",")))


def hosts_of(runs):
    """{cell label: [host record per (run, cell)]}. A cell's host record is
    shared by its arms (and by both paths of a bm_scan cell), so the first arm
    record of each (run, cell, path-or-not) stands for it."""
    cells = collections.OrderedDict()
    for run in runs:
        seen = set()
        for x in run["arms"]:
            h = x.get("host")
            name = x["cell"] if "cell" in x else x["shape"]
            key = (name, x.get("path") if "shape" in x else None)
            if h is None or key in seen:
                continue
            seen.add(key)
            cells.setdefault(name if "cell" in x else "%s/%s" % key, []).append(h)
    return cells


def t_hosts(a, runs):
    print("| run | 1-min load at the run gate | gate wait | load before / after the run | arm order | competing builds or test runs recorded in the run's cells |")
    print("|---|---|---|---|---|---|")
    for run in runs:
        comp = set()
        for x in run["arms"]:
            if x.get("host"):
                comp.update(x["host"]["competing"])
        print("| %d | %.2f | %.0f s | %s / %s | %s | %d |" % (
            run["run"], run["quiet_gate"]["load"], run["quiet_gate"]["waited_s"],
            run["host_before"]["loadavg"].split()[0], run["host_after"]["loadavg"].split()[0],
            " ".join(LABEL.get(x, x) for x in run.get("arm_order", [])), len(comp)))
    cells = hosts_of(runs)
    print("\n| cell | 1-min load at the cell start, min - max over runs | cell-runs above the load gate (%.1f) | "
          "cell-runs clean (siblings <= 20 %% busy; no other session's task above 15 %% of the pinned CPUs) | "
          "attempts, max | quiet wait before the cell, max | sibling CPUs, busy %% median [max] | "
          "other sessions' share of the server's / driver's CPU, %% max | "
          "run-queue wait %% of on-CPU time, median [max] (the server's own threads share its CPU) | "
          "cell-runs with a competing build or test run |" % GATE)
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    tot = collections.Counter()
    for cell, hs in cells.items():
        loads = [float(h["loadavg"].split()[0]) for h in hs]
        sib = [h.get("sibling_busy_pct") for h in hs if h.get("sibling_busy_pct") is not None]
        wt = [h.get("wait_pct") for h in hs if h.get("wait_pct") is not None]
        fs = [h.get("foreign_server_cpu_pct") for h in hs if h.get("foreign_server_cpu_pct") is not None]
        fd = [h.get("foreign_driver_cpu_pct") for h in hs if h.get("foreign_driver_cpu_pct") is not None]
        clean = sum(1 for h in hs if h.get("clean", False))
        tot["cells"] += len(hs)
        tot["above"] += sum(1 for x in loads if x > GATE)
        tot["clean"] += clean
        tot["comp"] += sum(1 for h in hs if h["competing"])
        print("| %s | %.2f - %.2f | %d of %d | %d of %d | %d | %.0f s | %s | %s | %s | %d |" % (
            cell, min(loads), max(loads), sum(1 for x in loads if x > GATE), len(loads), clean, len(hs),
            max(h.get("attempts", 1) for h in hs), max(h.get("quiet_waited_s", 0) for h in hs),
            ("%.1f [%.1f]" % (med(sib), max(sib))) if sib else "-",
            ("%.1f / %.1f" % (max(fs), max(fd))) if fs else "-",
            ("%.1f [%.1f]" % (med(wt), max(wt))) if wt else "-",
            sum(1 for h in hs if h["competing"])))
    print("\nall cell-runs: %d; above the load gate: %d; clean: %d; with a competing build or test run: %d" % (
        tot["cells"], tot["above"], tot["clean"], tot["comp"]))
    bad = sum(x["bad"] for run in runs for x in run["arms"] if "bad" in x)
    print("\nbad replies: %d" % bad)
    if "idle_cpu_us_per_s" in runs[0]:
        print("idle CPU per second per run and arm (µs): %s" % json.dumps(
            [{LABEL.get(k, k): round(v) for k, v in r["idle_cpu_us_per_s"].items()} for r in runs]))
    if "meta_self_lookups" in runs[0]:
        print("SHOW META's own lookups per run and arm: %s" % json.dumps(
            [r["meta_self_lookups"] for r in runs]))


def t_digest(a, runs):
    r0 = runs[0]
    arms = present(runs)
    bad_d, bad_a = [], []
    for cell, by in r0["digest"].items():
        if len(set(by.values())) != 1:
            bad_d.append((cell, by))
    print("row digests of one statement per cell, run 0: %d cells, %s" % (
        len(r0["digest"]), "all arms equal in every cell" if not bad_d else "DIFFER: %s" % bad_d))
    # ANALYZE: examined= and sel= must match across arms
    import re
    for cell, by in r0["analyze"].items():
        keys = {}
        for lab, text in by.items():
            keys[lab] = tuple(re.findall(r"(?:examined|matched|sel|pages|spills)=[0-9.%]+", text))
        if len(set(keys.values())) != 1:
            bad_a.append((cell, keys))
    print("ANALYZE of one statement per cell, run 0 (examined=, matched=, sel=, pages=, spills= tokens): %s" % (
        "equal across arms in every cell" if not bad_a else "DIFFER: %s" % bad_a))
    print("\nsample ANALYZE (bn, B): %s" % r0["analyze"].get("bn", {}).get("B", "-"))


def t_lookups(a, runs):
    arms = present(runs)
    print("| cell | path | " + " | ".join("%s lookups per scanned row (median over runs)" % LABEL[x] for x in arms) + " |")
    print("|---|---|" + "---|" * len(arms))
    cells = cell_info(runs)
    for cell in ("bn", "bn-recent", "rows-100k"):
        if cell not in cells:
            continue
        for path in ("literal",):
            row = []
            for x in arms:
                vals = [y["lookups"] / (y["ops"] * y["rows"]) for r in runs for y in r["arms"]
                        if y["arm"] == x and y["cell"] == cell and y["path"] == path and y["lookups"] is not None]
                row.append("%.4f" % med(vals) if vals else "no counter in this engine")
            print("| %s | %s | %s |" % (cell, path, " | ".join(row)))


# -- bn_census ---------------------------------------------------------------------

def bn_series(runs):
    """{(shape, path): {arm: [server_cpu_us per run]}} and the same for p50."""
    cpu = collections.OrderedDict()
    p50 = collections.OrderedDict()
    for run in runs:
        for x in run["arms"]:
            cpu.setdefault((x["shape"], x["path"]), collections.OrderedDict()).setdefault(
                x["bin"], []).append(x["server_cpu_us"])
            p50.setdefault((x["shape"], x["path"]), collections.OrderedDict()).setdefault(
                x["bin"], []).append(x["p50_us"])
    return cpu, p50


def t_bn_guard(a, files):
    for rows_label, path in files:
        data = jload(path)
        runs = data["runs"]
        arms = present(runs)
        cpu, p50 = bn_series(runs)
        print("\n**`tools/bn_census.py`, `--rows %s`: server CPU per statement (µs, median over %d runs "
              "[min, max]); statements/s is CPU-derived (1e6 / CPU µs); deltas paired by run**\n" % (
                  rows_label, len(runs)))
        cols = ["shape", "path"] + [LABEL[x] for x in arms] + [
            "A statements/s", "B statements/s", "B - A"] + (["B-off - A"] if "Boff" in arms else []) + (
            ["A-al64 - A (layout probe)"] if "Apad" in arms else []) + [
            "A - A' (S = max abs)", "B against S", "A wall p50", "B wall p50"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for (shape, path), by in cpu.items():
            row = [shape, path] + ["%s µs" % rng(by[x], "%.2f") for x in arms]
            row += ["%s /s" % format(round(1e6 / med(by["A"])), ","), "%s /s" % format(round(1e6 / med(by["B"])), ",")]
            pair = lambda x, y: [p - q for p, q in zip(by[x], by[y])]
            dba, daa = pair("B", "A"), pair("A", "A2")
            f = lambda d: "%s µs (%+.1f %%)" % (rng(d, "%+.2f"), 100.0 * med(d) / med(by["A"]))
            s, word = verdict(dba, daa)
            row += [f(dba)]
            if "Boff" in arms:
                row += [f(pair("Boff", "A"))]
            if "Apad" in arms:
                row += [f(pair("Apad", "A"))]
            row += ["%s µs (S = %.2f µs)" % (rng(daa, "%+.2f"), s), word]
            row += ["%.1f µs" % med(p50[(shape, path)]["A"]), "%.1f µs" % med(p50[(shape, path)]["B"])]
            print("| " + " | ".join(row) + " |")
        gt = collections.OrderedDict()
        for (shape, path), by in cpu.items():
            gt[(shape, path)] = sum(1 for p, q in zip(by["B"], by["A"]) if p > q)
        print("\nB slower than A in k of %d runs: %s" % (len(runs), ", ".join(
            "%s/%s %d" % (k[0], k[1], v) for k, v in gt.items())))
        print("end state and page growth: %s" % json.dumps(
            [{k: r.get(k) for k in ("end_state",)} for r in runs][0]))


def t_bn_pct(a, files):
    print("| rows | shape | path | arm | n | " + " | ".join(n for n, _ in PCTS) + " | statements/s at p50 (derived) |")
    print("|---|---|---|---|---|" + "---|" * (len(PCTS) + 1))
    for rows_label, path in files:
        data = jload(path)
        runs = data["runs"]
        arms = present(runs)
        # the bn_census records percentiles per run (p0/p25/p50/p90/p99, no p95): medians over runs
        by = collections.OrderedDict()
        for run in runs:
            for x in run["arms"]:
                by.setdefault((x["shape"], x["path"], x["bin"]), []).append(x)
        for (shape, pth, lab), xs in by.items():
            if lab not in arms:
                continue
            m = lambda k: med([x[k] for x in xs])
            print("| %s | %s | %s | %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | - | %.1f µs | %s /s |" % (
                rows_label, shape, pth, LABEL[lab], format(sum(x["ops"] for x in xs), ","),
                m("p0_us"), m("p25_us"), m("p50_us"), m("p90_us"), m("p99_us"),
                format(round(1e6 / m("p50_us")), ",")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["census", "bn"])
    ap.add_argument("file", help="census: one JSON; bn: ROWS=PATH[,ROWS=PATH...]")
    ap.add_argument("--pg")
    ap.add_argument("--pg2")
    ap.add_argument("--base", help="census --section net: BM-S1's census JSON (no arm field), literal column")
    ap.add_argument("--section", required=True)
    a = ap.parse_args()
    if a.mode == "census":
        runs = jload(a.file)["runs"]
        {"net": t_net, "delta": t_delta, "pg": t_pg, "rate": t_rate, "guard": t_guard_census, "pct": t_pct,
         "point": t_point_census, "hosts": t_hosts, "digest": t_digest, "lookups": t_lookups}[a.section](a, runs)
    else:
        files = [tuple(x.split("=", 1)) for x in a.file.split(",")]
        if a.section == "hosts":
            for rows_label, path in files:
                print("\n`--rows %s`\n" % rows_label)
                t_hosts(a, jload(path)["runs"])
        else:
            {"guard": t_bn_guard, "pct": t_bn_pct}[a.section](a, files)


if __name__ == "__main__":
    main()
