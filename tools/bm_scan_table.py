#!/usr/bin/env python3
"""Tables from `tools/bm_scan.py` and `tools/bn_pg_scan.py --shapes bm` output
(BM-S1, `workorder-bm-scan-row-kernel.md` §5.1).

    bm_scan_table.py census  CENSUS.json [--pg PG.json]
    bm_scan_table.py writers WRITERS.json
    bm_scan_table.py profile PROFILE.json [--parts tools/bn_parts.json] [--census CENSUS.json]

`census`: per cell and path, over the runs - server CPU per statement, the
point statement's on the same relation, the net row `(scan - point) / rows`
per run (median [min, max]), lookups per scanned row, and, pooled over every
run's statements, the wall-time percentiles; with `--pg` the twin's net row
and the KDS / PostgreSQL ratio. `writers`: the writer cell's paired
with/without comparison. `profile`: the sampler dumps folded into
`tools/bn_parts.json`'s parts, in ns per scanned row, and the BM-Q1 / BM-Q8
readings.
"""
import argparse, collections, json, os, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PCTS = (("p0", 0), ("p25", 25), ("p50", 50), ("p90", 90), ("p95", 95), ("p99", 99))


def pct(xs, p):
    xs = sorted(xs)
    if p == 0:
        return xs[0]
    return xs[max(0, min(len(xs) - 1, -(-p * len(xs) // 100) - 1))]


def med(xs):
    return statistics.median(xs)


def spread(xs, f="%.1f"):
    return (f + " [" + f + ", " + f + "]") % (med(xs), min(xs), max(xs))


def by_cell(runs):
    d = collections.OrderedDict()
    for run in runs:
        for arm in run["arms"]:
            d.setdefault((arm["cell"], arm["kind"], arm["rel"], arm["path"]), []).append(arm)
    return d


def net_rows(runs, path):
    """{cell: [net row ns per run]} for `path`."""
    point = {}
    out = collections.OrderedDict()
    for ri, run in enumerate(runs):
        for arm in run["arms"]:
            if arm["kind"] == "point" and arm["path"] == path:
                point[(ri, arm["rel"])] = arm["server_cpu_us"]
        for arm in run["arms"]:
            if arm["kind"] == "scan" and arm["path"] == path:
                p = point[(ri, arm["rel"])]
                out.setdefault(arm["cell"], []).append(
                    (arm["server_cpu_us"] - p) * 1e3 / arm["rows"])
    return out


def jload(path):
    """A driver's JSON, plain or gzip (the archive keeps `.json.gz`)."""
    import gzip
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt") as f:
        return json.load(f)


def pg_nets(path):
    """{cell: [net row ns per run]} and the raw runs of a `bn_pg_scan.py --shapes bm` file."""
    pg = jload(path)
    out = {}
    for run in pg["runs"]:
        pts = {x["rel"]: x["server_cpu_us"] for x in run["arms"] if x["kind"] == "point"}
        for x in run["arms"]:
            if x["kind"] == "scan":
                out.setdefault(x["cell"], []).append((x["server_cpu_us"] - pts[x["rel"]]) * 1e3 / x["rows"])
    return out, pg["runs"]


def census(a):
    runs = jload(a.file)["runs"]
    pg_net, pg_runs = pg_nets(a.pg) if a.pg else ({}, [])
    pg2_net, pg2_runs = pg_nets(a.pg2) if a.pg2 else ({}, [])
    import bm_scan
    cell_defs = {c.name: c for c in bm_scan.make_cells(argparse.Namespace(scan_ops=192))}
    cells = collections.OrderedDict()
    for run in runs:
        for arm in run["arms"]:
            if arm["kind"] == "scan":
                cells.setdefault(arm["cell"], arm)
    nl, npp = net_rows(runs, "literal"), net_rows(runs, "prepared")
    arms = by_cell(runs)
    want = lambda name: a.section in ("all", name)
    f = lambda xs: spread(xs) + " ns" if xs else "not measured"
    if want("net"):
        print("| cell | relation | rows scanned | rows out | KDS net row, literal | KDS net row, prepared "
              "| KDS scan rows/s (derived, literal) | lookups per scanned row | PostgreSQL net row, vacuumed "
              "| PostgreSQL net row, not vacuumed | KDS literal / PG vacuumed | KDS literal / PG not vacuumed |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for name, first in cells.items():
            L = arms[(name, "scan", first["rel"], "literal")]
            P = arms[(name, "scan", first["rel"], "prepared")]
            lk = [x["lookups"] / (x["ops"] * x["rows"]) for x in L + P]
            g1, g2 = pg_net.get(name), pg2_net.get(name)
            print("| %s | %s | %s | %d | %s | %s | %.1f M rows/s | %.4f | %s | %s | %s | %s |" % (
                name, first["rel"], format(first["rows"], ","), cell_defs[name].expect(1),
                f(nl[name]), f(npp[name]), 1e3 / med(nl[name]), med(lk), f(g1), f(g2),
                "%.2f" % (med(nl[name]) / med(g1)) if g1 else "-",
                "%.2f" % (med(nl[name]) / med(g2)) if g2 else "-"))
    if want("cpu"):
        print("| cell | KDS scan CPU, literal | KDS scan CPU, prepared | KDS point CPU, literal "
              "| PG scan CPU, vacuumed | PG point CPU, vacuumed |")
        print("|---|---|---|---|---|---|")
        for name, first in cells.items():
            L = arms[(name, "scan", first["rel"], "literal")]
            P = arms[(name, "scan", first["rel"], "prepared")]
            ptl = [x["server_cpu_us"] for k, v in arms.items() if k[1] == "point" and k[2] == first["rel"]
                   and k[3] == "literal" for x in v]
            sx = [x for r in pg_runs for x in r["arms"] if x["cell"] == name]
            ptx = [x["server_cpu_us"] for r in pg_runs for x in r["arms"]
                   if x["kind"] == "point" and sx and x["rel"] == sx[0]["rel"]]
            print("| %s | %s µs | %s µs | %s µs | %s | %s |" % (
                name, spread([x["server_cpu_us"] for x in L]), spread([x["server_cpu_us"] for x in P]),
                spread(ptl), (spread([x["server_cpu_us"] for x in sx]) + " µs") if sx else "-",
                (spread(ptx) + " µs") if ptx else "-"))
    if want("pct"):
        print("| cell | path | n | " + " | ".join(n for n, _ in PCTS)
              + " | statements/s at p50 (derived) |")
        print("|---|---|---|" + "---|" * (len(PCTS) + 1))
        for (name, kind, rel, path), xs in arms.items():
            lat = [v for x in xs for v in x["lat_us"]]
            print("| %s | %s | %s | %s | %s /s |" % (
                name if kind == "scan" else "point " + rel, path, format(len(lat), ","),
                " | ".join("%.1f µs" % pct(lat, p) for _, p in PCTS), format(round(1e6 / pct(lat, 50)), ",")))
        if pg_runs:
            pgarms = collections.OrderedDict()
            for run in pg_runs:
                for x in run["arms"]:
                    pgarms.setdefault(x["cell"], []).append(x)
            print("\nPostgreSQL 18.6, vacuumed, simple query protocol, pooled:\n")
            print("| cell | n | " + " | ".join(n for n, _ in PCTS) + " | statements/s at p50 (derived) |")
            print("|---|---|" + "---|" * (len(PCTS) + 1))
            for name, xs in pgarms.items():
                lat = [v for x in xs for v in x["lat_us"]]
                print("| %s | %s | %s | %s /s |" % (name, format(len(lat), ","),
                                                     " | ".join("%.1f µs" % pct(lat, p) for _, p in PCTS),
                                                     format(round(1e6 / pct(lat, 50)), ",")))
    if want("plans") and pg_runs:
        print("| cell | plan (first EXPLAIN, vacuumed run 0) |")
        print("|---|---|")
        for name, plan in pg_runs[0].get("plans", {}).items():
            print("| %s | `%s` |" % (name, plan.replace("|", "/")))
    if want("noise"):
        rep = jload(a.replicate)["runs"] if a.replicate else None
        rl, rp = (net_rows(rep, "literal"), net_rows(rep, "prepared")) if rep else ({}, {})
        print("| cell | net row, this set, literal | net row, replicate set, literal | this - replicate "
              "| this - replicate, prepared | paired literal - prepared, per run |")
        print("|---|---|---|---|---|---|")
        for name in cells:
            d = [x - y for x, y in zip(nl[name], npp[name])]
            print("| %s | %.1f ns | %s | %s | %s | %s ns |" % (
                name, med(nl[name]),
                ("%.1f ns" % med(rl[name])) if name in rl else "-",
                ("%+.2f ns" % (med(nl[name]) - med(rl[name]))) if name in rl else "-",
                ("%+.2f ns" % (med(npp[name]) - med(rp[name]))) if name in rp else "-",
                spread(d, "%+.2f")))
    if want("hosts"):
        print("| run | 1-min load at the gate | load before / after the run | cell loads min - max | competing processes recorded |")
        print("|---|---|---|---|---|")
        for run in runs:
            loads = [float(x["host"]["loadavg"].split()[0]) for x in run["arms"] if "host" in x]
            comp = {c for x in run["arms"] if "host" in x for c in x["host"]["competing"]}
            print("| %d | %.2f | %s / %s | %.2f - %.2f | %d |" % (
                run["run"], run["quiet_gate"]["load"], run["host_before"]["loadavg"].split()[0],
                run["host_after"]["loadavg"].split()[0], min(loads), max(loads), len(comp)))
        bad = sum(arm["bad"] for run in runs for arm in run["arms"])
        print("\nbad replies: %d; idle CPU per second per run (µs): %s" % (
            bad, [round(r["idle_cpu_us_per_s"]) for r in runs]))
        print("meta self lookups per run: %s" % [r["meta_self_lookups"] for r in runs])


def writer_segments(runs):
    """Segments kept for the writer cell: the scan session on core 1 and the
    writer's on core 0 before and after the segment (the load placer may move
    a session between two statements, BL-S3)."""
    ok = lambda b: b["scan_core_before"] == b["scan_core_after"] == 1 and b.get("writer_core", 0) == 0
    return [b for r in runs for b in r["blocks"] if ok(b)], sum(len(r["blocks"]) for r in runs)


def writers(a):
    """`a.file` may name several JSONs (comma-separated): their runs are pooled.
    With `--clean`, a run whose host record names a competing build (or a
    1-minute load above 3.5) before or after it is dropped."""
    runs_all = [r for f in a.file.split(",") for r in jload(f)["runs"]]
    if a.clean:
        def clean(r):
            comp = len(r["host_before"]["competing"]) + len(r["host_after"]["competing"])
            mx = max(float(r["host_before"]["loadavg"].split()[0]), float(r["host_after"]["loadavg"].split()[0]))
            return comp == 0 and mx <= 3.5
        dropped = [r for r in runs_all if not clean(r)]
        runs_all = [r for r in runs_all if clean(r)]
        print("runs kept %d, dropped for a competing build or load > 3.5: %d" % (len(runs_all), len(dropped)))
    by = collections.OrderedDict()
    for run in runs_all:
        by.setdefault(run["variant"], []).append(run)
    want = lambda name: a.section in ("all", name)
    for variant, runs in by.items():
        kept, total = writer_segments(runs)
        if want("summary"):
            print("\n`%s` writer:\n" % variant)
            print("| segments | scans | lookups per scanned row | scan core CPU per scan (median over segments) "
                  "| all-server CPU per scan | window latch waits per scan | window latch wait per scan | writer updates/s | "
                  + " | ".join(n for n, _ in PCTS) + " | scans/s at p50 (derived) |")
            print("|---|---|---|---|---|---|---|---|" + "---|" * (len(PCTS) + 1))
            for kind in ("alone", "writer"):
                ok = [b for b in kept if b["kind"] == kind]
                if not ok:
                    continue
                n_ops = sum(b["ops"] for b in ok)
                lat = [v for b in ok for v in b["lat_us"]]
                print("| %s: %d of %d | %s | %.4f | %.1f µs | %.1f µs | %.3f | %.3f µs | %s | %s | %s /s |" % (
                    "writer going" if kind == "writer" else "no writer", len(ok),
                    sum(1 for r in runs for b in r["blocks"] if b["kind"] == kind), format(n_ops, ","),
                    sum(b["lookups"] for b in ok) / (n_ops * 10000),
                    med([b["reactor_us_per_stmt"]["1"] for b in ok]),
                    med([b["server_total_us_per_stmt"] for b in ok]),
                    sum(b["window_waits"] for b in ok) / n_ops, sum(b["window_wait_us"] for b in ok) / n_ops,
                    ("%.0f /s" % (sum(b["updates"] for b in ok) / sum(b["wall_s"] for b in ok)))
                    if kind == "writer" else "0",
                    " | ".join("%.1f µs" % pct(lat, p) for _, p in PCTS), format(round(1e6 / pct(lat, 50)), ",")))
        if want("paired"):
            d_cpu, d_p50, d_p99 = [], [], []
            for r in runs:
                al = [b for b in writer_segments([r])[0] if b["kind"] == "alone"]
                wr = [b for b in writer_segments([r])[0] if b["kind"] == "writer"]
                if al and wr:
                    d_cpu.append(med([b["reactor_us_per_stmt"]["1"] for b in wr]) -
                                 med([b["reactor_us_per_stmt"]["1"] for b in al]))
                    d_p50.append(pct([v for b in wr for v in b["lat_us"]], 50) -
                                 pct([v for b in al for v in b["lat_us"]], 50))
                    d_p99.append(pct([v for b in wr for v in b["lat_us"]], 99) -
                                 pct([v for b in al for v in b["lat_us"]], 99))
            print("\n`%s`, paired by run, writer going minus no writer: scan core CPU %s µs, p50 %s µs, "
                  "p99 %s µs (%d runs)" % (variant, spread(d_cpu, "%+.1f"), spread(d_p50, "%+.1f"),
                                           spread(d_p99, "%+.0f"), len(d_cpu)))
            print("segments dropped for a session off its core: %d of %d; bad scans %d; bad updates %d" % (
                total - len(kept), total, sum(b["bad"] for r in runs for b in r["blocks"]),
                sum(b["update_bad"] for r in runs for b in r["blocks"])))
        if want("bins"):
            print("\n`%s`: scan core CPU by the share of rows that took the window path:\n" % variant)
            print("| lookups per scanned row | segments (no writer / writer going) | scan core CPU per scan, median [min, max] "
                  "| no writer, median | writer going, median | extra over the zero-lookup median, per lookup, no-writer segments only |")
            print("|---|---|---|---|---|---|")
            pts = [(b["lookups"] / (b["ops"] * 10000), b["reactor_us_per_stmt"]["1"], b["kind"]) for b in kept]
            zero = med([y for x, y, k in pts if x == 0]) if any(x == 0 for x, _, _ in pts) else None
            for lo, hi, label in ((0, 0, "0"), (1e-9, 0.05, "0 - 0.05"), (0.05, 0.5, "0.05 - 0.5"),
                                  (0.5, 0.95, "0.5 - 0.95"), (0.95, 5, "about 1 (every row)")):
                sel = [p for p in pts if lo <= p[0] <= hi]
                if not sel:
                    continue
                al = [p[1] for p in sel if p[2] == "alone"]
                wr = [p[1] for p in sel if p[2] == "writer"]
                extra = ""
                if zero is not None and lo > 0 and al:
                    mean_x = statistics.mean(p[0] for p in sel if p[2] == "alone")
                    extra = "%+.1f ns" % ((med(al) - zero) * 1e3 / (mean_x * 10000))
                print("| %s | %d / %d | %s µs | %s | %s | %s |" % (
                    label, len(al), len(wr), spread([p[1] for p in sel]),
                    ("%.1f µs" % med(al)) if al else "-", ("%.1f µs" % med(wr)) if wr else "-", extra or "-"))
        if want("series"):
            print("\n`%s`, lookups per scanned row by segment, run by run (a = no writer, w = writer going):\n" % variant)
            for run in runs:
                print("- run %d: %s" % (run["run"], " ".join("%s %.2f" % (b["kind"][0], b["lookups"] / (b["ops"] * 10000))
                                                                 for b in run["blocks"])))


def profile(a):
    import re
    import kds_sampler_report as rep
    data = jload(a.file)
    spec = json.load(open(a.parts))
    chained = {p: rx.startswith("chain:") for p, rx in spec}
    parts = [(p, re.compile(rx[len("chain:"):] if chained[p] else rx)) for p, rx in spec]
    census_net = {}
    if a.census:
        runs = jload(a.census)["runs"]
        census_net = {k: med(v) for k, v in net_rows(runs, "literal").items()}
    REPLACED = ("row walk: loop and callback", "row walk: per-row checks and counters",
                "row walk: copy", "row decode and encode", "row walk: residual")
    points = {r["cell"]: r for r in data["profile"] if r.get("kind") == "point"}
    infos = []
    for rec in data["profile"]:
        if "dump" not in rec:
            continue
        dump = rec["dump"]
        if a.dump_dir and not os.path.exists(dump):
            dump = os.path.join(a.dump_dir, os.path.basename(dump))
        maps, samples = rep.load(dump)
        names = rep.symbolize(maps, {pc for s in samples for pc in s})
        total = len(samples)
        chains = [[rep.trim(names[pc]) for pc in s] for s in samples]
        part_c = collections.Counter()
        lookup_incl = scan_lookup = scan_chain = 0
        for fn in chains:
            joined = "<".join(fn)
            got = "other"
            for p, rx in parts:
                if rx.search(joined) if chained[p] else any(rx.search(n) for n in fn):
                    got = p
                    break
            part_c[got] += 1
            if any("LookupCommit" in n for n in fn):
                lookup_incl += 1
                if any("AcceptTupleAt" in n for n in fn):
                    scan_lookup += 1
            if any(re.search(r"AcceptTupleAt|BtreeVisitLeafPage|ChainRunner|SelectRun", n) for n in fn):
                scan_chain += 1
        cpu = rec.get("server_cpu_us", rec.get("server_total_us_per_stmt"))
        rows = rec.get("rows", 10000)
        replaced = sum(part_c[p] for p in REPLACED) / max(1, total) * cpu * 1e3 / rows
        net = ((cpu - points[rec["cell"]]["server_cpu_us"]) * 1e3 / rows) if rec["cell"] in points else None
        infos.append({"rec": rec, "total": total, "part_c": part_c, "lookup_incl": lookup_incl,
                      "scan_lookup": scan_lookup, "scan_chain": scan_chain, "cpu": cpu, "rows": rows,
                      "replaced": replaced, "net": net,
                      "point": points[rec["cell"]]["server_cpu_us"] if rec["cell"] in points else None})
    if a.section == "compact":
        cells = [i for i in infos if i["rec"]["kind"] == "scan" and "writers" not in i["rec"]["cell"]]
        print("| part (`tools/bn_parts.json`, first match) | " + " | ".join(
            "%s: ns per scanned row (share)" % i["rec"]["cell"] for i in cells) + " |")
        print("|---|" + "---|" * len(cells))
        for p, _ in spec + [["other", None]]:
            print("| %s | %s |" % (p, " | ".join(
                "%.1f ns (%.1f %%)" % (i["part_c"][p] / max(1, i["total"]) * i["cpu"] * 1e3 / i["rows"],
                                       100.0 * i["part_c"][p] / max(1, i["total"])) for i in cells)))
        print("| samples | %s |" % " | ".join(format(i["total"], ",") for i in cells))
        print("| server CPU per scan, sampled | %s |" % " | ".join("%.1f µs" % i["cpu"] for i in cells))
        print("| server CPU per point statement, unsampled, same server | %s |" % " | ".join(
            "%.1f µs" % i["point"] for i in cells))
        print("| net row on the profiled build, (scan - point) / 10,000 | %s |" % " | ".join(
            "%.1f ns" % i["net"] for i in cells))
        print("| parts BM-S2..S4 replace (loop and callback, per-row checks and counters, copy, decode, "
              "residual) | %s |" % " | ".join("%.1f ns" % i["replaced"] for i in cells))
        print("| **replaced parts / net row (BM-Q1)** | %s |" % " | ".join(
            "**%.1f %%**" % (100.0 * i["replaced"] / i["net"]) for i in cells))
        print("| net row on the plain build (census, literal median) | %s |" % " | ".join(
            ("%.1f ns" % census_net[i["rec"]["cell"]]) if i["rec"]["cell"] in census_net else "-" for i in cells))
        return
    if a.section == "lookup":
        print("| dump | scans | scan-core reactor CPU per scan | samples | LookupCommit frames, all | "
              "LookupCommit under the scan walk | samples on the scan walk | LookupCommit / scan-walk samples "
              "| LookupCommit per scan (share x server CPU) | as a share of the scan core's CPU "
              "| window latch wait per scan | lookups per scanned row |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for i in infos:
            r = i["rec"]
            if "lookups" not in r:
                continue
            per_scan = i["scan_lookup"] / max(1, i["total"]) * i["cpu"]
            react = r["reactor_us_per_stmt"]["1"]
            print("| %s, %s | %s | %.1f µs | %s | %d (%.2f %%) | %d (%.2f %%) | %s | %.2f %% | %.1f µs "
                  "| %.2f %% | %.3f µs | %.3f |" % (
                      r["cell"], "writer going" if r["kind"] == "writer" else "no writer", format(r["ops"], ","),
                      react, format(i["total"], ","), i["lookup_incl"], 100.0 * i["lookup_incl"] / i["total"],
                      i["scan_lookup"], 100.0 * i["scan_lookup"] / i["total"], format(i["scan_chain"], ","),
                      100.0 * i["scan_lookup"] / max(1, i["scan_chain"]), per_scan, 100.0 * per_scan / react,
                      r.get("window_wait_us", 0) / r["ops"], r["lookups"] / (r["ops"] * 10000)))
        return
    for i in infos:
        rec, total, part_c, cpu, rows, replaced = (i["rec"], i["total"], i["part_c"], i["cpu"],
                                                    i["rows"], i["replaced"])
        print("\n### %s %s: %d samples, server CPU %.1f µs per statement, %d statements\n" % (
            rec["cell"], rec["kind"], total, cpu, rec["ops"]))
        print("| part | samples | share | ns per scanned row |")
        print("|---|---|---|---|")
        for p, _ in spec + [["other", None]]:
            c = part_c[p]
            print("| %s | %d | %.1f %% | %.1f |" % (p, c, 100.0 * c / max(1, total), c / max(1, total) * cpu * 1e3 / rows))
        print("\nreplaced parts: %.1f ns per scanned row, %.1f %% of the profile's statement" % (
            replaced, 100.0 * replaced / (cpu * 1e3 / rows)))
        if i["net"] is not None:
            print("net row on the profiled build %.1f ns -> replaced parts / net row = %.1f %%" % (
                i["net"], 100.0 * replaced / i["net"]))
        print("LookupCommit frames (inclusive): %d samples = %.2f %%; under the scan walk: %d = %.2f %%; "
              "samples on a scan-walk chain: %d" % (i["lookup_incl"], 100.0 * i["lookup_incl"] / max(1, total),
                                                    i["scan_lookup"], 100.0 * i["scan_lookup"] / max(1, total),
                                                    i["scan_chain"]))
        if "lookups" in rec:
            print("lookups in the window: %s; window latch wait µs: %s; reactor CPU per statement: %s" % (
                rec["lookups"], rec.get("window_wait_us"), rec.get("reactor_us_per_stmt")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["census", "writers", "profile"])
    ap.add_argument("file")
    ap.add_argument("--pg", help="census: the vacuumed PostgreSQL twin's JSON")
    ap.add_argument("--pg2", help="census: the not-vacuumed twin's JSON")
    ap.add_argument("--replicate", help="census --section noise: a second census JSON of the same binary")
    ap.add_argument("--section", default="all",
                    help="census: net, cpu, pct, plans, noise, hosts or all; writers: summary, "
                    "paired, bins, series or all; profile: compact, lookup or full")
    ap.add_argument("--parts", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    "bn_parts.json"))
    ap.add_argument("--census")
    ap.add_argument("--clean", action="store_true", help="writers: drop runs with a competing build")
    ap.add_argument("--dump-dir", help="profile: where the sampler dumps are, if not where the JSON says")
    a = ap.parse_args()
    {"census": census, "writers": writers, "profile": profile}[a.mode](a)


if __name__ == "__main__":
    main()
