#!/usr/bin/env python3
"""Markdown tables for the CD close from /home/cdkbs/bench-runs/cd-close/out/*.json."""
import json, os, re, statistics as st, sys

R = "/home/cdkbs/bench-runs/cd-close/out/"
med = st.median


def load(n):
    p = R + n + ".json"
    return json.load(open(p)) if os.path.exists(p) else None


def runs_of(n):
    d = load(n)
    return d["runs"] if d else []


def summ(name, A, A2, B, dec=2, unit_tps=False):
    dBA = [y - x for x, y in zip(A, B)]
    dAA = [abs(y - x) for x, y in zip(A, A2)]
    f = lambda v: f"{v:.{dec}f}"
    below = sum(1 for d in dBA if d < 0)
    verdict = "inside" if abs(med(dBA)) <= max(dAA) else "**OUTSIDE**"
    return (f"| {name} | {len(A)} | {f(med(A))} | {f(med(A2))} | {f(med(B))} | {f(med(dBA))} [{f(min(dBA))}, {f(max(dBA))}] | "
            f"{100 * med(dBA) / med(A):+.1f} % | {sum(1 for d in dBA if d > 0)} of {len(A)} | {f(med(dAA))} / {f(max(dAA))} | {verdict} |"), verdict, med(dBA), max(dAA)


HDR = ("| cell | n runs | A | A' | B | B - A paired, median [min, max] | % of A | B above A | A' - A typical / largest abs gap | B - A vs largest gap |\n"
       "|---|---|---|---|---|---|---|---|---|---|")


def shape_tables(files, label, shapes=None):
    runs = []
    for f in files:
        runs += runs_of(f)
    cells = {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["shape"], a["path"], a["bin"]), []).append(a)
    allshapes = shapes or sorted({k[0] for k in cells})
    print(f"\n#### {label}: server CPU per statement, us (insert/select-pk: one statement; update-range: one every-row UPDATE of 1,000 rows)\n")
    print(HDR)
    for sh in allshapes:
        for path in ("literal", "prepared"):
            if (sh, path, "A") not in cells:
                continue
            v = {arm: [x["server_cpu_us"] for x in cells[(sh, path, arm)]] for arm in ("A", "A2", "B")}
            print(summ(f"{sh} CPU us {path}", v["A"], v["A2"], v["B"])[0])
    print(f"\n#### {label}: statement wall latency, us; median over runs of each run's percentile\n")
    print("| shape | path | arm | ops | p0 | p25 | p50 | p90 | p99 | statements/s at mean (derived) | server CPU us | driver CPU us |\n|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sh in allshapes:
        for path in ("literal", "prepared"):
            for arm in ("A", "A2", "B"):
                if (sh, path, arm) not in cells:
                    continue
                L = cells[(sh, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {sh} | {path} | {arm} | {sum(x['ops'] for x in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6 / m('mean_us'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    es = all(all(v.values()) for r in runs for v in r.get("end_state", {}).values())
    unclean = sum(1 for r in runs for a in r["arms"] if not a["host"].get("clean", True))
    print(f"\nbad replies: {bad}; end_state all ok: {es}; n runs {len(runs)}; unclean cells (per arm-record): {unclean}")


def within_arm(files):
    """Within-arm: insert-dropped minus insert-spare (same SPARE 2 layout), and minus insert-plain, per arm."""
    runs = []
    for f in files:
        runs += runs_of(f)
    cells = {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["shape"], a["path"], a["bin"]), []).append(a["server_cpu_us"])
    print("\n#### within-arm: insert-dropped minus insert-spare, server CPU us per statement (pooled u1+u2, paired by run)\n")
    print("| path | arm | n | insert-plain | insert-spare | insert-dropped | dropped - spare, median [min, max] | % |\n|---|---|---|---|---|---|---|---|")
    for path in ("literal", "prepared"):
        for arm in ("A", "A2", "B"):
            pl, sp, dr = (cells[(s, path, arm)] for s in ("insert-plain", "insert-spare", "insert-dropped"))
            d = [x - y for x, y in zip(dr, sp)]
            print(f"| {path} | {arm} | {len(d)} | {med(pl):.2f} | {med(sp):.2f} | {med(dr):.2f} | {med(d):+.2f} [{min(d):+.2f}, {max(d):+.2f}] | {100 * med(d) / med(sp):+.1f} % |")


def scan_tables(files, label):
    runs = []
    for f in files:
        runs += runs_of(f)
    cells, relof = {}, {}
    for r in runs:
        for a in r["arms"]:
            cells.setdefault((a["cell"], a["path"], a["arm"]), []).append(a)
            if a["kind"] == "scan":
                relof[a["cell"]] = (a["rel"], a["rows"])
    print(f"\n#### {label}: net scan row, ns of server CPU per row, (scan CPU - point SELECT CPU on the same relation) / rows; point SELECT CPU in us\n")
    print(HDR)
    for cell, (rel, rows) in relof.items():
        for path in ("literal", "prepared"):
            v = {arm: [(s["server_cpu_us"] - p["server_cpu_us"]) / rows * 1000
                       for s, p in zip(cells[(cell, path, arm)], cells[("point:" + rel, path, arm)])] for arm in ("A", "A2", "B")}
            print(summ(f"{cell} ({rows}) net row ns {path}", v["A"], v["A2"], v["B"])[0])
    for cell, (rel, rows) in relof.items():
        for path in ("literal", "prepared"):
            v = {arm: [p["server_cpu_us"] for p in cells[("point:" + rel, path, arm)]] for arm in ("A", "A2", "B")}
            print(summ(f"point:{rel} CPU us {path}", v["A"], v["A2"], v["B"])[0])
    print(f"\n#### {label}: latency of one scan (wall, client side), us; median over runs of each run's percentile\n")
    print("| cell | path | arm | ops | p0 | p25 | p50 | p95 | p99 | scans/s at p50 (derived) | server CPU us/scan |\n|---|---|---|---|---|---|---|---|---|---|---|")
    for cell in relof:
        for path in ("literal", "prepared"):
            for arm in ("A", "A2", "B"):
                L = cells[(cell, path, arm)]
                m = lambda k: med([x[k] for x in L])
                print(f"| {cell} | {path} | {arm} | {sum(x['ops'] for x in L)} | {m('p_p0'):.1f} | {m('p_p25'):.1f} | {m('p_p50'):.1f} | {m('p_p95'):.1f} | {m('p_p99'):.1f} | {1e6 / m('p_p50'):.0f} | {m('server_cpu_us'):.1f} |")
    bad = sum(a["bad"] for r in runs for a in r["arms"])
    wait = sum(a["window_wait_us"] for r in runs for a in r["arms"])
    miss = sum(a["pool_misses"] for r in runs for a in r["arms"] if a["kind"] == "scan")
    dig = sorted({(c, len(set(d.values()))) for r in runs for c, d in r["digest"].items()})
    unclean = [(i, a["cell"], a["path"], a["arm"]) for i, r in enumerate(runs) for a in r["arms"] if a.get("clean") is False]
    print(f"\nbad replies: {bad}; window_wait_us total: {wait}; scan pool_misses total: {miss}; digest arms-distinct per cell: {dig}; n runs {len(runs)}; unclean windows: {len(unclean)} {unclean[:4]}")


def per_file_range(files):
    print("\n#### update-range and insert-dropped by driver invocation: B - A and A' - A, server CPU us per statement, paired by run\n")
    print("| file | start order | shape | path | runs | A | A' | B | B - A median [min, max] | A' - A median [min, max] |\n|---|---|---|---|---|---|---|---|---|---|")
    order = {"u1": "B, A', A", "u2": "A, A', B", "u3": "A', B, A", "u4": "B, A, A'"}
    for f in files:
        runs = runs_of(f)
        if not runs:
            continue
        for sh in ("update-range", "insert-dropped"):
            if sh == "insert-dropped" and f in ("u3", "u4"):
                pass
            for path in ("literal", "prepared"):
                v = {arm: [next(a["server_cpu_us"] for a in r["arms"] if a["shape"] == sh and a["path"] == path and a["bin"] == arm) for r in runs] for arm in ("A", "A2", "B")}
                dB = [y - x for x, y in zip(v["A"], v["B"])]
                dA = [y - x for x, y in zip(v["A"], v["A2"])]
                print(f"| {f} | {order[f]} | {sh} | {path} | {len(runs)} | {med(v['A']):.1f} | {med(v['A2']):.1f} | {med(v['B']):.1f} | {med(dB):+.1f} [{min(dB):+.1f}, {max(dB):+.1f}] | {med(dA):+.1f} [{min(dA):+.1f}, {max(dA):+.1f}] |")


def num(tag, key):
    m = re.search(r"(\d+) " + key, tag)
    return int(m.group(1)) if m else None


def pass_tables():
    d = load("p4b")
    if d:
        print("\n#### cell 4: PURGE DROPPED COLUMNS FROM p (binary B), by relation size; wall is the client's, one statement\n")
        print("| rows | rep | load s | dirty pass wall | rows/s (dirty) | ns/row | server CPU s | WAL bytes | WAL bytes/row | rows cleaned | spills released | clean re-run wall | rows/s (all clean) | ns/row | WAL bytes (re-run) | reusing ADD us | host clean |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for r in d["cell4"]:
            x, c = r["dirty"], r["clean"]
            n = r["rows"]
            print(f"| {n:,} | {r['rep']} | {r['load_s']:.1f} | {x['wall_s'] * 1e3:.2f} ms | {n / x['wall_s']:,.0f} | {x['wall_s'] / n * 1e9:.0f} | {x['server_cpu_s']:.3f} | {x['wal_bytes']:,} | {x['wal_bytes'] / n:.1f} | "
                  f"{num(x['tag'], 'row')} | {num(x['tag'], 'spill')} | {c['wall_s'] * 1e3:.3f} ms | {n / c['wall_s']:,.0f} | {c['wall_s'] / n * 1e9:.0f} | {c['wal_bytes']} | {r['reusing_add_us']:.0f} | {x['host']['clean'] and c['host']['clean']} |")
        print("\nreply tags (rep 0):")
        for r in d["cell4"]:
            if r["rep"] == 0:
                print(f"- {r['rows']:,}: dirty `{r['dirty']['tag']}`; re-run `{r['clean']['tag']}`; ADD `{r['add_tag']}`; rows reading NULL in the reused column: {r['rows_null_in_new_col']}")
    d = load("p5b")
    if d:
        print("\n#### cell 5: serial autocommit point-UPDATE stream (1 connection, literal) around PURGE DROPPED COLUMNS on a 1,000,000-row relation; latency us\n")
        print("| rep | window | ops | stmts/s | p0 | p25 | p50 | p95 | p99 | max |\n|---|---|---|---|---|---|---|---|---|---|")
        for r in d["cell5"]:
            for w, rate in (("pre (3 s)", r["pre_stmts_per_s"]), ("during pass", None), ("post (3 s)", r["post_stmts_per_s"]), ("whole run", None)):
                k = {"pre (3 s)": "pre", "during pass": "during", "post (3 s)": "post", "whole run": "whole"}[w]
                p = r[k]
                print(f"| {r['rep']} | {w} | {p['n']:,} | {'' if rate is None else f'{rate:,.0f}'} | {p['p0']:.1f} | {p['p25']:.1f} | {p['p50']:.1f} | {p['p95']:.1f} | {p['p99']:.1f} | {p['max']:.0f} |")
        print("\n| rep | pass wall | pass server CPU | pass reply | re-run reply | top-5 stream latencies, ms | bad replies |\n|---|---|---|---|---|---|---|")
        for r in d["cell5"]:
            print(f"| {r['rep']} | {r['pass_wall_s'] * 1e3:.1f} ms | {r['pass_server_cpu_s'] * 1e3:.1f} ms | `{r['pass_tag']}` | `{r['rerun_tag']}` | {r['top5_latencies_ms']} | {r['bad']} |")
    c = load("p5c")
    if c:
        print("\n#### cell 5 control: the same stream and windows, PURGE not sent (a 2.0 s idle gap in its place); latency us\n")
        print("| rep | window | ops | stmts/s | p0 | p25 | p50 | p95 | p99 | max |\n|---|---|---|---|---|---|---|---|---|---|")
        for r in c["cell5"]:
            for w, k, rate in (("pre (3 s)", "pre", r["pre_stmts_per_s"]), ("gap (2 s)", "during", None), ("post (3 s)", "post", r["post_stmts_per_s"]), ("whole run", "whole", None)):
                p = r[k]
                print(f"| {r['rep']} | {w} | {p['n']:,} | {'' if rate is None else f'{rate:,.0f}'} | {p['p0']:.1f} | {p['p25']:.1f} | {p['p50']:.1f} | {p['p95']:.1f} | {p['p99']:.1f} | {p['max']:.0f} |")
    for lab, dd in (("pass", d), ("control", c)):
        if not dd:
            continue
        print(f"\n#### cell 5 ({lab}): stream statements over 5 ms, [offset from the pass start (or the gap start) s, latency ms]\n")
        for r in dd["cell5"]:
            print(f"- rep {r['rep']}: {r['stalls_over_5ms_[offset_from_pass_start_s, latency_ms]']}")
    d4 = load("p4b")
    if d4:
        keys = ["wal_rolls", "wal_roll_hold_us", "wal_roll_sync_us", "wal_roll_sync_segments", "wal_ring_full", "sched_hold_out_run_longest_us", "sched_hold_in_run_longest_us", "sched_foreground_longest_poll_us", "pool_hits"]
        print("\n#### cell 4: SHOW META deltas around the dirty pass (the waits)\n")
        print("| rows | rep | wall ms | server CPU ms | WAL segment rolls | roll hold ms (fsync of the segment left, under the latch) | ring-full waits | longest non-yielding run ms | pool hits |\n|---|---|---|---|---|---|---|---|---|")
        for r in d4["cell4"]:
            m = r["dirty"]["meta_delta"]
            print(f"| {r['rows']:,} | {r['rep']} | {r['dirty']['wall_s'] * 1e3:.0f} | {r['dirty']['server_cpu_s'] * 1e3:.0f} | {m.get('wal_rolls', 0)} | {m.get('wal_roll_hold_us', 0) / 1e3:.0f} | {m.get('wal_ring_full', 0)} | {max(m.get('sched_hold_out_run_longest_us', 0), m.get('sched_hold_in_run_longest_us', 0)) / 1e3:.0f} | {m.get('pool_hits', 0):,} |")
    if d:
        print("\n#### cell 5 (pass): SHOW META deltas over the pass\n")
        print("| rep | wall ms | WAL rolls | roll hold ms | ring-full | longest non-yielding run ms |\n|---|---|---|---|---|---|")
        for r in d["cell5"]:
            m = r["meta_delta_pass"]
            print(f"| {r['rep']} | {r['pass_wall_s'] * 1e3:.0f} | {m.get('wal_rolls', 0)} | {m.get('wal_roll_hold_us', 0) / 1e3:.0f} | {m.get('wal_ring_full', 0)} | {max(m.get('sched_hold_out_run_longest_us', 0), m.get('sched_hold_in_run_longest_us', 0)) / 1e3:.0f} |")
    d = load("p6b")
    if d:
        c = d["cell6"][0]
        print(f"\n#### cell 6: ADD COLUMN, wall us, {c['trials']} trials each (3-row relations, interleaved)\n")
        print("| ADD kind | n | p0 | p25 | p50 | p95 | p99 | max | statements/s at mean (derived) | server CPU us (median) |\n|---|---|---|---|---|---|---|---|---|---|")
        for k, key, cpu in (("vacant slot", "vacant", "vacant_server_cpu_us_median"), ("reusing a clean dropped slot", "reusing", "reusing_server_cpu_us_median")):
            p = c[key]
            print(f"| {k} | {p['n']} | {p['p0']:.1f} | {p['p25']:.1f} | {p['p50']:.1f} | {p['p95']:.1f} | {p['p99']:.1f} | {p['max']:.1f} | {1e6 / p['mean']:,.0f} | {c[cpu]:.1f} |")
        v, r = c["raw_us"]["vacant"], c["raw_us"]["reusing"]
        dd = sorted(y - x for x, y in zip(v, r))
        q = lambda xs, f: xs[int(f * (len(xs) - 1))]
        print(f"\nPaired by trial (reusing - vacant, adjacent in time, order alternating): median {med(dd):+.1f} us, p25 {q(dd, .25):+.1f}, p75 {q(dd, .75):+.1f}, "
              f"reusing slower in {sum(1 for x in dd if x > 0)} of {len(dd)}. Vacant trials split by position: first-in-pair median "
              f"{med(v[0::2]):.1f}, second-in-pair {med(v[1::2]):.1f}; reusing first-in-pair {med(r[1::2]):.1f}, second-in-pair {med(r[0::2]):.1f} (pairs alternate order).")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    if what in ("u", "all"):
        shape_tables(["u1", "u2", "u3", "u4"], "u1-u4 pooled (18 runs), 1,000 rows", ["insert", "insert-dropped", "update-range"])
        per_file_range(["u1", "u2", "u3", "u4"])
        shape_tables(["u1", "u2"], "u1+u2 pooled, 1,000 rows")
        within_arm(["u1", "u2"])
        for f in ("u1", "u2"):
            if runs_of(f):
                shape_tables([f], f"{f} alone")
    if what in ("s", "all"):
        if runs_of("s1"):
            scan_tables(["s1", "s2"], "scan 10,000 rows (bn), s1+s2 pooled")
            scan_tables(["s1"], "scan 10,000 rows (bn), s1 alone (start order A, A', B)")
            scan_tables(["s2"], "scan 10,000 rows (bn), s2 alone (start order B, A', A)")
        if runs_of("z"):
            scan_tables(["z"], "scan 200 / 1,000 rows")
    if what in ("p", "all"):
        pass_tables()
