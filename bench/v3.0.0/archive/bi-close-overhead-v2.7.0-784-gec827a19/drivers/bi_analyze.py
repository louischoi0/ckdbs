#!/usr/bin/env python3
"""BI close overhead A/B: tables from the per-run summaries (never the raw per-statement files).
usage: bi_analyze.py <out-dir> <section> <cell> [<cell> ...]
sections: matrix | dist | longest | ckpt | host | sync
A cell is a directory <out-dir>/<cell>/ holding n<rows>-r<run>.json.  "B - A" is the second server minus the
first; for the aa-* cells both servers run the same binary (A), so every delta there is the floor."""
import glob, json, os, random, re, statistics as st, sys, time

OUT, SECTION = sys.argv[1], sys.argv[2]
CELLS = [a for a in sys.argv[3:] if "=" not in a]
OPTS = dict(a.split("=", 1) for a in sys.argv[3:] if "=" in a)
SIZES = (200, 1000, 10000)
DESC = {"insert": "INSERT, pk omitted", "insert-again": "INSERT again (replicate)", "select-pk": "SELECT by pk",
        "update-pk": "UPDATE by pk", "delete-pk": "DELETE by pk", "bulk20": "bulk INSERT, 20 rows",
        "range100": "whole-relation walk (COUNT/MIN/MAX, 100-id window)", "ping": "SHOW META",
        "ping2": "SHOW NAMESPACES (control)", "iso": "SET ISOLATION LEVEL (control)"}
TIMED = ["insert", "insert-again", "select-pk", "update-pk", "delete-pk", "bulk20", "range100"]
random.seed(1)


def boot(vals, fn, n=4000):
    b = sorted(fn(random.choices(vals, k=len(vals))) for _ in range(n))
    return b[int(0.025 * n)], b[int(0.975 * n)]


def load(cell, rows):
    fs = [f for f in glob.glob(f"{OUT}/{cell}/n{rows}-r*.json")]
    fs.sort(key=lambda f: int(re.search(r"-r(\d+)\.json", f).group(1)))
    return [json.load(open(f)) for f in fs]


def arms_of(summ):
    return [a for a in DESC if a in summ[0]["arms"]["A"]]


def matrix(cell):
    data = {}
    for r in SIZES:
        summ = load(cell, r)
        if not summ:
            continue
        for a in arms_of(summ):
            d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
            dq = [(s["arms"]["B"][a]["qps"] / s["arms"]["A"][a]["qps"] - 1) * 100 for s in summ]
            data[(a, r)] = (st.median(d50), boot(d50, st.median), st.median(dq), boot(dq, st.median),
                            st.median(s["arms"]["A"][a]["qps"] for s in summ), st.median(s["arms"]["B"][a]["qps"] for s in summ),
                            st.median(s["arms"]["A"][a]["p50_us"] for s in summ), st.median(s["arms"]["B"][a]["p50_us"] for s in summ), len(summ),
                            st.pstdev(d50) if len(d50) > 1 else 0.0)
    seen = [a for a in DESC if any((a, r) in data for r in SIZES)]
    print(f"\n#### {cell}: p50 delta B - A (95 % CI of the median over runs) and tps change\n")
    print("| shape | " + " | ".join(f"{r:,} rows" for r in SIZES) + " |")
    print("|---|" + "---|" * len(SIZES))
    for a in seen:
        row = []
        for r in SIZES:
            v = data.get((a, r))
            row.append("-" if not v else f"{v[0]:+.2f} µs [{v[1][0]:+.2f}, {v[1][1]:+.2f}]; {v[2]:+.1f} % tps [{v[3][0]:+.1f}, {v[3][1]:+.1f}]")
        print(f"| {DESC[a]} | " + " | ".join(row) + " |")
    if OPTS.get("only") == "delta":
        return
    print(f"\n#### {cell}: throughput, A then B (median of the runs' ops / elapsed), and p50, A then B\n")
    print("| shape | " + " | ".join(f"{r:,} rows" for r in SIZES) + " |")
    print("|---|" + "---|" * len(SIZES))
    for a in seen:
        row = []
        for r in SIZES:
            v = data.get((a, r))
            row.append("-" if not v else f"{v[4]:,.0f} -> {v[5]:,.0f} tps; p50 {v[6]:.1f} -> {v[7]:.1f} µs")
        print(f"| {DESC[a]} | " + " | ".join(row) + " |")
    print(f"\n(runs per cell: {sorted({v[8] for v in data.values()})}; sd of the per-run p50 delta, insert: " +
          ", ".join(f"{r:,} rows {data[('insert', r)][9]:.2f} µs" for r in SIZES if ('insert', r) in data) + ")")


def dist(cell):
    print(f"\n#### {cell}: latency distribution, median over runs of each run's percentile, A then B\n")
    print("| rows | statement | server | statements | p0 | p25 | p50 | p95 | p99 |")
    print("|---|---|---|---|---|---|---|---|---|")
    want = OPTS.get("arms", "insert,select-pk,update-pk,delete-pk,bulk20,range100,iso").split(",")
    for r in [int(x) for x in OPTS.get("rows", "200,1000,10000").split(",")]:
        summ = load(cell, r)
        if not summ:
            continue
        for a in arms_of(summ):
            if a not in want:
                continue
            for srv in "AB":
                m = lambda k: st.median(s["arms"][srv][a][k] for s in summ)
                n = sum(s["arms"][srv][a]["ops"] for s in summ)
                print(f"| {r:,} | {DESC[a]} | {srv} | {n:,} statements | {m('p0_us'):.1f} µs | {m('p25_us'):.1f} µs | {m('p50_us'):.1f} µs | {m('p95_us'):.1f} µs | {m('p99_us'):.1f} µs |")


def longest(cell):
    print(f"\n#### {cell}: the longest single statement of any timed arm, per run (controls excluded)\n")
    print("| rows | runs | A: median [min-max] | A: runs over 10 ms / over 40 ms | B: median [min-max] | B: runs over 10 ms / over 40 ms | B - A per run: median (95 % CI) | arm holding the longest, A / B |")
    print("|---|---|---|---|---|---|---|---|")
    for r in SIZES:
        summ = load(cell, r)
        if not summ:
            continue
        mx = {"A": [], "B": []}
        hold = {"A": {}, "B": {}}
        for s in summ:
            for srv in "AB":
                best = max(((s["arms"][srv][a]["max_us"], a) for a in TIMED if a in s["arms"][srv]))
                mx[srv].append(best[0] / 1000)
                hold[srv][best[1]] = hold[srv].get(best[1], 0) + 1
        diff = [b - a for a, b in zip(mx["A"], mx["B"])]
        f = lambda v: f"{st.median(v):.1f} ms [{min(v):.1f}-{max(v):.1f}]"
        c = lambda v: f"{sum(x > 10 for x in v)} of {len(v)} runs / {sum(x > 40 for x in v)} of {len(v)} runs"
        h = lambda d: ", ".join(f"{k} {v}" for k, v in sorted(d.items(), key=lambda kv: -kv[1])[:3])
        ci = boot(diff, st.median)
        print(f"| {r:,} | {len(summ)} runs | {f(mx['A'])} | {c(mx['A'])} | {f(mx['B'])} | {c(mx['B'])} | {st.median(diff):+.1f} ms [{ci[0]:+.1f}, {ci[1]:+.1f}] | {h(hold['A'])} / {h(hold['B'])} |")
    print(f"\n#### {cell}: the longest statement of each arm (max_us), median over runs, A then B\n")
    print("| rows | " + " | ".join(DESC[a].split(" (")[0] for a in TIMED) + " |")
    print("|---|" + "---|" * len(TIMED))
    for r in SIZES:
        summ = load(cell, r)
        if not summ:
            continue
        row = []
        for a in TIMED:
            if a not in summ[0]["arms"]["A"]:
                row.append("-")
                continue
            va = st.median(s["arms"]["A"][a]["max_us"] for s in summ) / 1000
            vb = st.median(s["arms"]["B"][a]["max_us"] for s in summ) / 1000
            row.append(f"{va:.2f} -> {vb:.2f} ms")
        print(f"| {r:,} | " + " | ".join(row) + " |")


def ckpt(cell):
    print(f"\n#### {cell}: checkpoint runs of the session's core during the arms, and the run record at the end of each run\n")
    print("| rows | server | session core | checkpoint runs landing in the arms: median [min-max] | runs with at least one | last run's wall time (`checkpoint_last_wall_us`): median / max | last run's steps: median | longest reactor iteration inside a run (`sched_hold_in_run_longest_us`): median / max | iterations over 10 ms inside runs, sum | `pool_resident` max / frames | `pool_refused`, sum |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in SIZES:
        summ = load(cell, r)
        if not summ or "snaps" not in summ[0]:
            continue
        for srv in "AB":
            d, wall, steps, hold, h10, res, ref, cores = [], [], [], [], [], [], [], set()
            last_arm = list(summ[0]["snaps"][srv])[-1]
            for s in summ:
                sn = s["snaps"][srv]
                d.append(sn[last_arm]["checkpoint_runs"] - sn["start"]["checkpoint_runs"])
                m = s["meta_end"][srv]
                wall.append(m.get("checkpoint_last_wall_us", 0))
                steps.append(m.get("checkpoint_last_steps", 0))
                hold.append(m.get("sched_hold_in_run_longest_us", 0))
                h10.append(m.get("sched_hold_in_run_over_10ms", 0))
                res.append(s["pool_end"][srv]["pool_resident"])
                ref.append(s["pool_end"][srv]["pool_refused"])
                cores.add(int(sn["start"]["core"]))
            print(f"| {r:,} | {srv} | core {'/'.join(str(c) for c in sorted(cores))} | {st.median(d):.0f} runs [{min(d):.0f}-{max(d):.0f}] | {sum(x >= 1 for x in d)} of {len(d)} runs | {st.median(wall)/1000:.1f} ms / {max(wall)/1000:.1f} ms | {st.median(steps):.0f} steps | {st.median(hold)/1000:.1f} ms / {max(hold)/1000:.1f} ms | {sum(h10):.0f} iterations | {max(res):,.0f} / {summ[0]['frames']:,} frames | {sum(ref):.0f} refusals |")


def holds(cell):
    print(f"\n#### {cell}: the session core's longest reactor iteration (`SHOW META`, counters since boot), A then B\n")
    print("| rows | server | inside a checkpoint run, whole run: median / max | outside every run, during setup (load): median / max | outside every run, grown during the arms: runs of N / largest | statement-level: the arm holding the run (checkpoint_runs first increased), its max_us: median / max |")
    print("|---|---|---|---|---|---|")
    for r in SIZES:
        summ = load(cell, r)
        if not summ or "snaps" not in summ[0]:
            continue
        for srv in "AB":
            inrun, outset, outgrow, grown, armmax = [], [], [], 0, []
            for s in summ:
                sn = s["snaps"][srv]
                inrun.append(s["meta_end"][srv].get("sched_hold_in_run_longest_us", 0) / 1000)
                o0 = sn["start"]["sched_hold_out_run_longest_us"] / 1000
                o1 = list(sn.values())[-1]["sched_hold_out_run_longest_us"] / 1000
                outset.append(o0)
                if o1 > o0:
                    grown += 1
                    outgrow.append(o1)
                prev = sn["start"]["checkpoint_runs"]
                for arm, v in sn.items():
                    if arm == "start":
                        continue
                    if v["checkpoint_runs"] > prev:
                        armmax.append(s["arms"][srv][arm]["max_us"] / 1000)
                        break
            f = lambda v: "-" if not v else f"{st.median(v):.1f} ms / {max(v):.1f} ms"
            print(f"| {r:,} | {srv} | {f(inrun)} | {f(outset)} | {grown} of {len(summ)} runs / {max(outgrow) if outgrow else 0:.1f} ms | {f(armmax)} ({len(armmax)} runs) |")


def longestall():
    print("| cell | rows | runs | A: longest statement of a run, median [min-max] | A: runs over 10 ms / over 40 ms | B: median [min-max] | B: runs over 10 ms / over 40 ms | B - A per run: median (95 % CI) |")
    print("|---|---|---|---|---|---|---|---|")
    for cell in CELLS:
        for r in SIZES:
            summ = load(cell, r)
            if not summ:
                continue
            mx = {"A": [], "B": []}
            for s in summ:
                for srv in "AB":
                    mx[srv].append(max(s["arms"][srv][a]["max_us"] for a in TIMED if a in s["arms"][srv]) / 1000)
            diff = [b - a for a, b in zip(mx["A"], mx["B"])]
            ci = boot(diff, st.median)
            f = lambda v: f"{st.median(v):.1f} ms [{min(v):.1f}-{max(v):.1f}]"
            c = lambda v: f"{sum(x > 10 for x in v)} of {len(v)} runs / {sum(x > 40 for x in v)} of {len(v)} runs"
            print(f"| `{cell}` | {r:,} | {len(summ)} runs | {f(mx['A'])} | {c(mx['A'])} | {f(mx['B'])} | {c(mx['B'])} | {st.median(diff):+.1f} ms [{ci[0]:+.1f}, {ci[1]:+.1f}] |")


REAL = re.compile(r"^\d+ (\S*/)?(cc1plus|cmake|ctest|kds_tests|c\+\+|g\+\+|ld|cc1)\b")


def host(cell):
    fs = [f for f in glob.glob(f"{OUT}/{cell}/n*-r*.json")]
    if not fs:
        print(f"| {cell} | no run files |")
        return
    lb, la, real, anyc, mx, errs, ts, waits = [], [], 0, 0, 0.0, 0, [], []
    for f in fs:
        s = json.load(open(f))
        lb.append(float(s["host_before"]["loadavg"].split()[0]))
        la.append(float(s["host_after"]["loadavg"].split()[0]))
        comp = s["host_before"].get("competing", []) + s["host_after"].get("competing", [])
        comp = [c for c in comp if "claude-ccef-cwd" not in c and "claude-96a3-cwd" not in c and "shell-snapshots" not in c]
        anyc += bool(comp)
        real += any(REAL.match(x) for x in comp)
        for a in ("iso", "ping2"):
            if a in s["arms"]["A"]:
                mx = max(mx, abs(s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"]))
        errs += sum(x.get("errors", 0) for srv in "AB" for x in s["arms"][srv].values())
        ts.append(os.path.getmtime(f))
        waits.append(s["wait_s"])
    t0, t1 = (time.strftime("%H:%M:%S", time.gmtime(t)) for t in (min(ts), max(ts)))
    print(f"| `{cell}` | {len(fs)} runs | {t0}-{t1} UTC | {min(lb):.2f}-{max(lb):.2f} | {min(la):.2f}-{max(la):.2f} | {real} of {len(fs)} runs | {anyc} of {len(fs)} runs | {max(waits):.0f} s | {errs} errors | {mx:.2f} µs |")


def sync(cell):
    print(f"\n#### {cell}: synced INSERT, B - A p50 (95 % CI), tps change, and A then B throughput\n")
    print("| rows | shape | A p50 | B - A p50 (95 % CI) | B - A as share of A's p50 | tps change (95 % CI) | A -> B throughput | sd of per-run p50 delta |")
    print("|---|---|---|---|---|---|---|---|")
    for r in SIZES:
        summ = load(cell, r)
        if not summ:
            continue
        for a in ("insert", "insert-again", "iso"):
            if a not in summ[0]["arms"]["A"]:
                continue
            d50 = [s["arms"]["B"][a]["p50_us"] - s["arms"]["A"][a]["p50_us"] for s in summ]
            dq = [(s["arms"]["B"][a]["qps"] / s["arms"]["A"][a]["qps"] - 1) * 100 for s in summ]
            pa = st.median(s["arms"]["A"][a]["p50_us"] for s in summ)
            c, q = boot(d50, st.median), boot(dq, st.median)
            qa = st.median(s["arms"]["A"][a]["qps"] for s in summ)
            qb = st.median(s["arms"]["B"][a]["qps"] for s in summ)
            print(f"| {r:,} | {DESC[a]} | {pa:.1f} µs | {st.median(d50):+.2f} µs [{c[0]:+.2f}, {c[1]:+.2f}] | {st.median(d50)/pa*100:+.2f} % | {st.median(dq):+.1f} % [{q[0]:+.1f}, {q[1]:+.1f}] | {qa:,.0f} -> {qb:,.0f} tps | {st.pstdev(d50):.2f} µs |")


if SECTION == "longestall":
    longestall()
    sys.exit(0)
for cell in CELLS:
    {"matrix": matrix, "dist": dist, "longest": longest, "ckpt": ckpt, "host": host, "sync": sync, "holds": holds}[SECTION](cell)
