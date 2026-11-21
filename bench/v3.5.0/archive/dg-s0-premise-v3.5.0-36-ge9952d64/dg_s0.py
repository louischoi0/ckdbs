#!/usr/bin/env python3
"""DG-S0's tables, from out/r<run>/<cell>/ beside this file: the driver's JSON
(tools/bl_mixed.py at e9952d64), the server log's per-roll lines, and the
spike's fill.log (M4: each body's reserve, write and fsync, each ask, each
pooled take, each removed segment; CLOCK_MONOTONIC ns, the driver's clock).

Every figure is the median over the runs of a per-run figure, with the runs'
range beside it where it matters. Only events inside the measured window
count. A "long statement" is an inserter statement of 30 ms or more.
Usage: dg_s0.py [out-dir]  (default: ./out)
"""
import glob
import json
import math
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "out")
KV = re.compile(r"(\w+)=(-?\d+)")
LONG_NS = 30_000_000
PACE_US = 8000  # dg-s0.sh's pace=8000
# In a paced cell the fill spans most of a segment's life, so the "in a fill"
# shares say nothing about cause there; they are printed for it as "n/a".
ARMS = ["base", "nofill", "pw", "pc", "pace"]
SHAPES = [("bg", "bulk writer, `group`"), ("br", "bulk writer, `relaxed`"),
          ("mg", "M1, `group`"), ("ms", "M1, `strict`"), ("gg", "M1, 16 writers, `group`")]


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def rank(xs, p):
    xs = sorted(xs)
    return xs[max(0, math.ceil(p * len(xs)) - 1)] if xs else None


def load(d):
    js = glob.glob(os.path.join(d, "*.json"))
    if not js:
        return None
    r = json.load(open(js[0]))
    w0, w1 = r["measure_from_mono_ns"], r["stop_at_mono_ns"]
    r["_w"] = (w0, w1)
    rolls = []
    log = js[0][:-5] + ".s.log"
    if os.path.exists(log):
        for line in open(log):
            if "[wal] roll" in line:
                kv = {k: int(v) for k, v in KV.findall(line)}
                m = re.search(r"body=(\w+)", line)
                kv["_body"] = m.group(1) if m else "?"
                if w0 <= kv.get("start_ns", 0) <= w1:
                    rolls.append(kv)
    r["_rolls"] = rolls
    fills, asks, removed, pooled, pool_built = [], [], [], [], None
    fl = os.path.join(d, "fill.log")
    if os.path.exists(fl):
        for line in open(fl):
            kv = {k: int(v) for k, v in KV.findall(line)}
            if line.startswith("fill "):
                fills.append(kv)
            elif line.startswith("ask "):
                asks.append(kv)
            elif line.startswith("removed "):
                removed.append(kv)
            elif line.startswith("pooled "):
                pooled.append(kv)
            elif line.startswith("pool "):
                pool_built = kv
    inw = lambda t: w0 <= t <= w1  # noqa: E731
    # A fill counts if it overlaps the window.
    r["_fills"] = [f for f in fills if f["end_ns"] >= w0 and f["start_ns"] <= w1]
    r["_fills_all"] = fills
    r["_asks"] = [a for a in asks if inw(a["at_ns"])]
    r["_removed"] = [x for x in removed if inw(x["at_ns"])]
    r["_pooled"] = pooled
    r["_pool_built"] = pool_built
    r["_paced"] = os.path.basename(d).endswith("-pace")
    longs = []
    for c in r["clients"]:
        if c["role"] == "inserter":
            for t, ns in c["slow"]:
                if ns >= LONG_NS and t >= w0 and t + ns <= w1:
                    longs.append((t, t + ns))
    r["_long"] = sorted(longs)
    return r


def overlap(a, b):
    return max(0, min(a[1], b[1]) - max(a[0], b[0]))


def per_run(r):
    w0, w1 = r["_w"]
    secs = (w1 - w0) / 1e9
    holds = sorted(x["hold_us"] for x in r["_rolls"])
    gate = sum(x["sync_gate_us"] for x in r["_rolls"])
    fills = [(f["start_ns"], f["end_ns"]) for f in r["_fills"] if f.get("ahead") == 1]
    long_ns = sum(e - s for s, e in r["_long"])
    # The share of long-statement time that lies inside a fill built ahead.
    in_fill = sum(overlap(l, f) for l in r["_long"] for f in fills)
    gate_rolls = [x for x in r["_rolls"] if x["sync_gate_us"] >= 10_000]
    gate_in_fill = sum(1 for x in gate_rolls
                       if any(overlap((x["start_ns"], x["end_ns"]), f) > 0 for f in fills))
    bodies = {}
    for x in r["_rolls"]:
        bodies[x["_body"]] = bodies.get(x["_body"], 0) + 1
    ws, wf = r.get("writer_service", {}), r.get("writer_from_schedule", {})
    ins = r.get("inserter", {})
    return {
        "rolls": len(r["_rolls"]),
        "hold_p50_ms": (rank(holds, 0.5) or 0) / 1e3 if holds else None,
        "hold_max_ms": holds[-1] / 1e3 if holds else None,
        "holds_ge80": sum(1 for h in holds if h >= 80_000),
        "gate_share": gate / sum(holds) if holds and sum(holds) else None,
        "header_ms": med([x["header_us"] / 1e3 for x in r["_rolls"]]),
        "bodies": bodies,
        "commit_p50": wf.get("p50_us"), "commit_p95": wf.get("p95_us"), "commit_p99": wf.get("p99_us"),
        "svc_p50": ws.get("p50_us"), "svc_p95": ws.get("p95_us"), "svc_p99": ws.get("p99_us"),
        "commits": ws.get("n"),
        "ins_p50": ins.get("p50_us"), "ins_p99": ins.get("p99_us"), "ins_max": ins.get("max_us"),
        "rows_s": r.get("inserter_rows_per_s"), "log_mb_s": (r.get("log_bytes_per_s") or 0) / 1e6,
        "long_n": len(r["_long"]), "long_ms_per_s": long_ns / 1e6 / secs,
        "long_in_fill": None if r["_paced"] else (in_fill / long_ns if long_ns else None),
        "gate_rolls": len(gate_rolls), "gate_rolls_in_fill": None if r["_paced"] else gate_in_fill,
        "read_p99": r.get("read_from_schedule", {}).get("p99_us"),
        "walker_n": r.get("walker", {}).get("n"), "walker_p99": r.get("walker", {}).get("p99_us"),
        # The paced arm's write loop includes its 64 sleeps of PACE_US; they
        # are taken out, so the figure is the loop's I/O and syscall time.
        "fill_write_ms": med([(f["write_us"] - (64 * PACE_US if r["_paced"] else 0)) / 1e3
                              for f in r["_fills"] if f.get("ahead") == 1]),
        "pool_gap_s": ((w0 - r["_pool_built"]["at_ns"]) / 1e9 if r["_pool_built"] else None),
        "fill_fsync_ms": med([f["fsync_us"] / 1e3 for f in r["_fills"] if f.get("ahead") == 1]),
        "fills_ahead": sum(1 for f in r["_fills"] if f.get("ahead") == 1),
        "fills_inline": sum(1 for f in r["_fills"] if f.get("ahead") == 0),
        "removed": len(r["_removed"]),
        "pool_left": r["_pooled"][-1]["left"] if r["_pooled"] else None,
        "pool_built": (r["_pool_built"] or {}).get("built"),
        "errors": r.get("errors"), "load": r["host_before"]["loadavg"].split()[0],
    }


def runs_of(cell):
    out = []
    for d in sorted(glob.glob(os.path.join(OUT, "r*", cell))):
        r = load(d)
        if r:
            out.append(per_run(r))
    return out


def f(x, nd=1):
    if x is None:
        return "-"
    if isinstance(x, float):
        return f"{x:.{nd}f}"
    return str(x)


def cellrow(cell, keys):
    rs = runs_of(cell)
    if not rs:
        return None
    vals = []
    for k, nd in keys:
        xs = [r[k] for r in rs if r[k] is not None]
        if not xs:
            vals.append("-")
            continue
        m = statistics.median(xs)
        vals.append(f"{f(float(m), nd)} [{f(float(min(xs)), nd)}-{f(float(max(xs)), nd)}]"
                    if len(xs) > 1 else f(float(m), nd))
    return len(rs), vals


def table(title, shapes, keys, heads):
    print(f"\n## {title}\n")
    print("| cell | runs | " + " | ".join(heads) + " |")
    print("|" + "---|" * (len(heads) + 2))
    for sh in shapes:
        for arm in ARMS:
            got = cellrow(f"{sh}-{arm}", keys)
            if got:
                print(f"| {sh}-{arm} | {got[0]} | " + " | ".join(got[1]) + " |")


def main():
    print("# DG-S0 tables (median [min-max] over runs)")
    table("1. The commit (M1): writers' latency from the schedule and service, µs", ["mg", "ms", "gg"],
          [("commit_p50", 0), ("commit_p95", 0), ("commit_p99", 0), ("svc_p50", 0), ("svc_p95", 0),
           ("commits", 0)],
          ["sched p50", "sched p95", "sched p99", "svc p50", "svc p95", "commits"])
    table("2. The inserter (a commit of a block or more in M1; the bulk writer in bg/br)",
          ["mg", "ms", "gg", "bg", "br"],
          [("ins_p50", 0), ("ins_p99", 0), ("ins_max", 0), ("rows_s", 0), ("log_mb_s", 1),
           ("long_n", 0), ("long_ms_per_s", 1), ("long_in_fill", 2)],
          ["p50 µs", "p99 µs", "max µs", "rows/s", "log MB/s", "long stmts", "long ms/s",
           "long share in a fill"])
    table("3. The roll", ["bg", "br", "mg", "ms", "gg"],
          [("rolls", 0), ("hold_p50_ms", 1), ("hold_max_ms", 1), ("holds_ge80", 0), ("gate_share", 2),
           ("header_ms", 2), ("gate_rolls", 0), ("gate_rolls_in_fill", 0)],
          ["rolls", "hold p50 ms", "hold max ms", "holds >= 80 ms", "gate share", "header sync ms",
           "rolls gated >= 10 ms", "of them in a fill"])
    table("4. The others: reader p99 from the schedule, walker", ["bg", "br", "mg", "ms", "gg"],
          [("read_p99", 0), ("walker_n", 0), ("walker_p99", 0)],
          ["reader p99 µs", "walker stmts", "walker p99 µs"])
    table("5. M4: the fill timed, and supply", ["bg", "br", "mg", "ms", "gg"],
          [("fills_ahead", 0), ("fills_inline", 0), ("fill_write_ms", 1), ("fill_fsync_ms", 1),
           ("removed", 0), ("pool_built", 0), ("pool_left", 0), ("pool_gap_s", 1)],
          ["fills ahead", "fills inline", "write ms", "fsync ms", "segments removed",
           "pool built", "pool left", "pool built to window s"])
    print("\n## 6. Body outcomes at the roll, runs summed\n")
    for sh, _ in SHAPES:
        for arm in ARMS:
            rs = runs_of(f"{sh}-{arm}")
            if rs:
                tot = {}
                for r in rs:
                    for k, v in r["bodies"].items():
                        tot[k] = tot.get(k, 0) + v
                print(f"- {sh}-{arm}: {tot}; errors {[r['errors'] for r in rs]}; "
                      f"load at start {[r['load'] for r in rs]}")


if __name__ == "__main__":
    main()
