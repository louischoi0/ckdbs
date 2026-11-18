#!/usr/bin/env python3
"""CZ-S1b's tables (raft-marks-2026-11-18.md, the operator's W26): C2 priced
further, from out/r<run>/<cell>/*.json and *.s.log beside this file
(tools/bl_mixed.py with the spike's `--stall-cores` and `--insert-rate`;
cz-s1b.sh names the cells). The loaders are copied from CZ-S1's `cz_s1.py`,
so that this archive stands alone; section 4 reads CZ-S1's archive, which
lies beside this one.

A "stall sample" is a reader sample of 10 ms or more from its scheduled
instant. A writer's latency is from its scheduled instant too (open loop).
"""
import glob
import json
import math
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
CELLS = ["iso", "c1h", "c1c2h", "c1x", "c1c2x", "g-base", "g-c2", "s-base", "s-c2", "gb-base", "gb-c2"]
HELD = ("c1h", "c1c2h", "c1x", "c1c2x")  # the `levels` cell, the inserter held to 120 (h) and 150 (x) statements a second;
                                          # the x cells ran in a second pass, after the others, with no `iso` of their own
COMMIT = ("g-base", "g-c2", "s-base", "s-c2")  # readers and one-row writers
BULK = ("gb-base", "gb-c2")      # the `levels` cell at `group`, the inserter closed-loop
S1 = os.path.join(HERE, "..", "cz-s1-ceiling-v3.4.12-66-g20d89234", "out")
RUNS = sorted(int(os.path.basename(p)[1:]) for p in glob.glob(f"{HERE}/out/r*") if os.path.isdir(p))
SEG = 64 << 20          # the log's segment size, bytes
STALL_NS = 10_000_000   # a stall sample
LONG_US = 10_000        # a phase long enough to make one
EPISODE_GAP_NS = 50_000_000
EDGE_NS = 5_000_000     # how close to a run's edge counts as at it
KV = re.compile(r"(\w+)=(-?\d+)")


def fmt(us):
    return f"{us / 1e3:.1f} ms" if us >= 1e3 else f"{us:.0f} µs"


def rank(xs, p):
    """Nearest-rank percentile of a sorted list, the driver's rule."""
    return xs[max(0, math.ceil(p * len(xs)) - 1)]


def load(cell, r):
    hits = glob.glob(f"{HERE}/out/r{r}/{cell}/*.json")
    if not hits:
        return None
    d = json.load(open(hits[0]))
    d["_ck"], d["_rl"], d["_an"] = [], [], []
    for line in open(hits[0][:-5] + ".s.log"):
        kv = {k: int(v) for k, v in KV.findall(line)}
        if "[checkpoint] run:" in line:
            d["_ck"].append(kv)
        elif "[wal] roll" in line:
            m = re.search(r"body=(\w+)", line)
            kv["_body"] = m.group(1) if m else None
            d["_rl"].append(kv)
        elif "[checkpoint] anchor published:" in line:
            d["_an"].append(kv)
    return d


DATA = {c: [(r, d) for r in RUNS if (d := load(c, r))] for c in CELLS}


def runs(cell):
    return DATA[cell]


def window(d):
    return d["measure_from_mono_ns"], d["stop_at_mono_ns"]


def inside(iv, w):
    return iv["start_ns"] < w[1] and iv["end_ns"] > w[0]


def overlapping(a, b, ivs):
    return [iv for iv in ivs if iv["start_ns"] < b and iv["end_ns"] > a]


def readers(d):
    return [c for c in d["clients"] if c["role"] == "reader"]


def read_cores(d):
    return sorted(int(c) for c in d["meta_by_core"])


def stalls(d):
    """[due ns, latency ns, core or None] of every stall sample, by time."""
    return sorted(([s[0], s[1], int(s[2]) if len(s) > 2 and s[2] is not None else None]
                   for c in readers(d) for s in c["slow"] if s[1] >= STALL_NS), key=lambda s: s[:2])


def covered(ivs, w):
    """The share of the window some interval of `ivs` covers (union)."""
    spans = sorted((max(iv["start_ns"], w[0]), min(iv["end_ns"], w[1])) for iv in ivs if inside(iv, w))
    total, end = 0, w[0]
    for s, e in spans:
        if e > end:
            total += e - max(s, end)
            end = e
    return 100.0 * total / (w[1] - w[0])


def episodes(d):
    """Stall samples grouped where their intervals lie within the gap of one
    another: [start, end, samples, the samples' cores]."""
    eps = []
    for t0, ns, core in stalls(d):
        if eps and t0 <= eps[-1][1] + EPISODE_GAP_NS:
            eps[-1][1] = max(eps[-1][1], t0 + ns)
            eps[-1][2] += 1
            eps[-1][3].append(core)
        else:
            eps.append([t0, t0 + ns, 1, [core]])
    return eps


def live_segments(d, w):
    """The segment being written at the window's end, less the segment of the
    lowest redo start among each core's latest anchor line before it, plus
    one. A roll line's `seg` is the segment the roll entered. An anchor line
    carries no monotonic stamp; each is written just before its core's run
    line, so it is dated by that run's end, and a core's first is the mount's.
    """
    ends, seen, latest = {}, {}, {}
    for iv in d["_ck"]:
        ends.setdefault(iv["core"], []).append(iv["end_ns"])
    for a in d["_an"]:
        k = seen.get(a["core"], 0)
        seen[a["core"]] = k + 1
        mine = ends.get(a["core"], [])
        if k == 0 or (k <= len(mine) and mine[k - 1] <= w[1]):
            latest[a["core"]] = a["redo_start"]
    cur = max((iv["seg"] for iv in d["_rl"] if iv["end_ns"] <= w[1]), default=0)
    return cur - min(latest.values()) // SEG + 1 if latest else None


def host(d):
    b, a = d["host_before"], d["host_after"]
    return bool(b["competing"] or a["competing"]) or float(b["loadavg"].split()[0]) >= 2


def med(xs):
    return statistics.median(xs)


def summ(d, key):
    return d.get(key) or {}


print("### 0. The host at each cell (bench/README.md rule 4)")
print("\n`build` marks a cell with a `cc1plus|cmake --build|ctest` process at either end or a load of 2 or more at its start.")
print("\n| cell | run | driver wait | load at start | load at end | competing at start | competing at end | marked | `cz_mode` | errors |")
print("|---|---|---|---|---|---|---|---|---|---|")
for cell in CELLS:
    for r, d in runs(cell):
        b, a = d["host_before"], d["host_after"]
        modes = sorted({iv.get("cz_mode") for iv in d["_ck"]})
        print(f"| {cell} | r{r} | {d['wait_s']:.0f} s | {b['loadavg'].split()[0]} | {a['loadavg'].split()[0]} | "
              f"{len(b['competing'])} processes | {len(a['competing'])} processes | {'build' if host(d) else '-'} | {modes} | {d['errors']} errors |")

print("\n### 1. C1 against C1 with C2 at one load (the inserter held to a rate, `relaxed`): `h` 120 statements a second, `x` 150")
print("\nThe `x` cells ran in a second pass, 5 runs after the first pass ended; their pairs are against the first pass's `iso` of the same run number, which is not interleaved with them.")
print("\n#### 1.1 per run")
print("| cell | run | reader p50 | reader p99 | reader p99.9 | reader max | stall samples | inserter rate | log rate | walker statements | walker p99 | rolls in window | roll hold median | roll hold max | in-run holds > 10 ms, read cores | marked |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
P99 = {}
for cell in ("iso",) + HELD:
    for r, d in runs(cell):
        p = d["read_from_schedule"]
        P99.setdefault(cell, {})[r] = p["p99_us"]
        w = window(d)
        rl = sorted(iv["hold_us"] for iv in d["_rl"] if inside(iv, w))
        rc = read_cores(d)
        holds = sum(int(d["meta_by_core"][str(c)].get("sched_hold_in_run_over_10ms", 0)) for c in rc)
        wk = summ(d, "walker")
        print(f"| {cell} | r{r} | {fmt(p['p50_us'])} | {fmt(p['p99_us'])} | {fmt(p['p99.9_us'])} | {fmt(p['max_us'])} | {len(stalls(d))} samples | "
              f"{d.get('inserter_rows_per_s') or 0:,.0f} rows/s | {(d.get('log_bytes_per_s') or 0) / 1e6:.1f} MB/s | {wk.get('n', 0)} statements | "
              f"{fmt(wk.get('p99_us', 0))} | {len(rl)} rolls | {fmt(med(rl)) if rl else '-'} | {fmt(rl[-1]) if rl else '-'} | {holds} holds | {'build' if host(d) else '-'} |")

print("\n#### 1.2 medians of runs, and CZ-Q1 (a)'s two clauses")
print("| cell | reader p99 (median) | p99 range | against `iso`'s median | pairs within 2x | pairs with no build on either side | p99.9 (median) | stall samples, all runs | inserter rate (median) | log rate (median) | walker statements (median) | walker p99 (median) | roll hold median, pooled | read cores' stream-latch wait, all runs |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for cell in ("iso",) + HELD:
    rs = runs(cell)
    v = P99[cell]
    pairs = [r for r in v if r in P99["iso"]]
    within = sum(v[r] <= 2 * P99["iso"][r] for r in pairs)
    iso = dict(runs("iso"))
    clean = [f"{v[r] / P99['iso'][r]:.2f}x" for r, d in rs if r in iso and not host(d) and not host(iso[r])]
    rl = sorted(iv["hold_us"] for _, d in rs for iv in d["_rl"] if inside(iv, window(d)))
    lw = sum(d["meta_delta"].get(f"load_c{c}_waited_wal_stream_us", 0) for _, d in rs for c in read_cores(d))
    print(f"| {cell} | {fmt(med(v.values()))} | {fmt(min(v.values()))} - {fmt(max(v.values()))} | {med(v.values()) / med(P99['iso'].values()):.2f}x | "
          f"{within} of {len(pairs)} | {', '.join(clean) or '-'} | {fmt(med([d['read_from_schedule']['p99.9_us'] for _, d in rs]))} | "
          f"{sum(len(stalls(d)) for _, d in rs)} samples | {med([d.get('inserter_rows_per_s') or 0 for _, d in rs]):,.0f} rows/s | "
          f"{med([(d.get('log_bytes_per_s') or 0) / 1e6 for _, d in rs]):.1f} MB/s | {med([summ(d, 'walker').get('n', 0) for _, d in rs]):.0f} statements | "
          f"{fmt(med([summ(d, 'walker').get('p99_us', 0) for _, d in rs]))} | {fmt(med(rl)) if rl else '-'} | {fmt(lw)} |")

print("\n### 2. The commit path with and without the zero prewrite: 6 readers and 4 one-row writers at 800 a second")
print("\n#### 2.1 per run")
print("| cell | run | writer p50 | writer p99 | writer p99.9 | writer max | writer service p50 | writer service p99 | writes kept | reader p99 | writer syncs | group batches, all cores | commits a batch | rolls | marked |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for cell in COMMIT:
    for r, d in runs(cell):
        ws, wv, md = d["writer_from_schedule"], d["writer_service"], d["meta_delta"]
        # `wal_group_*` is each core's own tally, and `meta_delta` carries only
        # the core that answered the control session (any core, `--ctl-core
        # -1`): summed over `meta_by_core`, which reads all four cores in
        # these cells. `strict` counts no group commit, so it has no ratio.
        gb = sum(c.get("wal_group_batches", 0) for c in d["meta_by_core"].values())
        gc = sum(c.get("wal_group_commits", 0) for c in d["meta_by_core"].values())
        print(f"| {cell} | r{r} | {fmt(ws['p50_us'])} | {fmt(ws['p99_us'])} | {fmt(ws['p99.9_us'])} | {fmt(ws['max_us'])} | {fmt(wv['p50_us'])} | "
              f"{fmt(wv['p99_us'])} | {ws['n']} writes | {fmt(d['read_from_schedule']['p99_us'])} | {int(md.get('contention_syncs_writer', 0))} syncs | "
              f"{int(gb)} batches | {f'{gc / gb:.1f}' if gc and gb else '-'} | "
              f"{len(d['_rl'])} rolls | {'build' if host(d) else '-'} |")

print("\n#### 2.2 medians of runs")
print("| cell | writer p50 | writer p99 | writer p99.9 | writer service p50 | writer service p99 | reader p99 | writer syncs a window | runs marked `build` |")
print("|---|---|---|---|---|---|---|---|---|")
for cell in COMMIT:
    rs = runs(cell)
    q = lambda k, f: med([d[k][f] for _, d in rs])
    print(f"| {cell} | {fmt(q('writer_from_schedule', 'p50_us'))} | {fmt(q('writer_from_schedule', 'p99_us'))} | {fmt(q('writer_from_schedule', 'p99.9_us'))} | "
          f"{fmt(q('writer_service', 'p50_us'))} | {fmt(q('writer_service', 'p99_us'))} | {fmt(q('read_from_schedule', 'p99_us'))} | "
          f"{med([d['meta_delta'].get('contention_syncs_writer', 0) for _, d in rs]):.0f} syncs | {sum(host(d) for _, d in rs)} of {len(rs)} |")

print("\n#### 2.2a the writer's p95, which separates the arms where the median and the p99 do not")
print("| cell | service p95 (median) | service p95, each run | from-schedule p95 (median) | writes 1.5 ms or more late, each run |")
print("|---|---|---|---|---|")
for cell in COMMIT:
    rs = runs(cell)
    sv = [d["writer_service"]["p95_us"] for _, d in rs]
    sc = [d["writer_from_schedule"]["p95_us"] for _, d in rs]
    late = []
    for _, d in rs:
        ws = [c for c in d["clients"] if c["role"] == "writer"]
        n = sum(c["ok"] for c in ws)
        late.append(100.0 * sum(1 for c in ws for x in c["slow"] if x[1] >= 1_500_000) / max(1, n))
    print(f"| {cell} | {fmt(med(sv))} | {', '.join(fmt(x) for x in sv)} | {fmt(med(sc))} | {', '.join(f'{x:.1f} %' for x in late)} |")

print("\n#### 2.3 the pairs: C2 against the base, medians")
print("| durability | writer p50, base | writer p50, C2 | change | writer p99, base | writer p99, C2 | change | writer service p50, base | C2 | change |")
print("|---|---|---|---|---|---|---|---|---|---|")
for name, b, c in (("group", "g-base", "g-c2"), ("strict", "s-base", "s-c2")):
    cols = []
    for k, f in (("writer_from_schedule", "p50_us"), ("writer_from_schedule", "p99_us"), ("writer_service", "p50_us")):
        x, y = med([d[k][f] for _, d in runs(b)]), med([d[k][f] for _, d in runs(c)])
        cols.append(f"{fmt(x)} | {fmt(y)} | {100.0 * (y / x - 1):+.1f} %")
    print(f"| {name} | " + " | ".join(cols) + " |")

print("\n#### 2.4 the same over the runs no build reached")
print("| cell | runs | writer p50 (median) | writer p50, each | writer service p50 (median) | writer p99 (median) | writer p99, each | writer p99.9 (median) | writer max, the largest |")
print("|---|---|---|---|---|---|---|---|---|")
for cell in COMMIT:
    rs = [(r, d) for r, d in runs(cell) if not host(d)]
    f = lambda k, x: [d[k][x] for _, d in rs]
    print(f"| {cell} | {', '.join(f'r{r}' for r, _ in rs)} | {fmt(med(f('writer_from_schedule', 'p50_us')))} | "
          f"{', '.join(fmt(x) for x in f('writer_from_schedule', 'p50_us'))} | {fmt(med(f('writer_service', 'p50_us')))} | "
          f"{fmt(med(f('writer_from_schedule', 'p99_us')))} | {', '.join(fmt(x) for x in f('writer_from_schedule', 'p99_us'))} | "
          f"{fmt(med(f('writer_from_schedule', 'p99.9_us')))} | {fmt(max(f('writer_from_schedule', 'max_us')))} |")

print("\n#### 2.5 the held-rate cells over the runs no build reached")
print("| cell | runs | reader p99 (median) | reader p99, each | walker statements (median) | walker p99 (median) | stall samples | roll hold median, pooled | roll hold max |")
print("|---|---|---|---|---|---|---|---|---|")
for cell in ("iso",) + HELD:
    rs = [(r, d) for r, d in runs(cell) if not host(d)]
    rl = sorted(iv["hold_us"] for _, d in rs for iv in d["_rl"] if inside(iv, window(d)))
    v = [d["read_from_schedule"]["p99_us"] for _, d in rs]
    print(f"| {cell} | {', '.join(f'r{r}' for r, _ in rs)} | {fmt(med(v))} | {', '.join(fmt(x) for x in v)} | "
          f"{med([summ(d, 'walker').get('n', 0) for _, d in rs]):.0f} statements | {fmt(med([summ(d, 'walker').get('p99_us', 0) for _, d in rs]))} | "
          f"{sum(len(stalls(d)) for _, d in rs)} samples | {fmt(med(rl)) if rl else '-'} | {fmt(rl[-1]) if rl else '-'} |")

print("\n### 3. The bulk writer at `group`: the `levels` cell, the inserter closed-loop")
print("\n| cell | run | inserter rate | inserter p50 | inserter p99 | inserter max | log rate | walker statements | walker p99 | reader p99 | stall samples | rolls in window | roll hold median | roll hold max | marked |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for cell in BULK:
    for r, d in runs(cell):
        w = window(d)
        rl = sorted(iv["hold_us"] for iv in d["_rl"] if inside(iv, w))
        i, wk = summ(d, "inserter"), summ(d, "walker")
        print(f"| {cell} | r{r} | {d.get('inserter_rows_per_s') or 0:,.0f} rows/s | {fmt(i.get('p50_us', 0))} | {fmt(i.get('p99_us', 0))} | {fmt(i.get('max_us', 0))} | "
              f"{(d.get('log_bytes_per_s') or 0) / 1e6:.1f} MB/s | {wk.get('n', 0)} statements | {fmt(wk.get('p99_us', 0))} | {fmt(d['read_from_schedule']['p99_us'])} | "
              f"{len(stalls(d))} samples | {len(rl)} rolls | {fmt(med(rl)) if rl else '-'} | {fmt(rl[-1]) if rl else '-'} | {'build' if host(d) else '-'} |")
print("\n#### 3.1 medians of runs")
print("| cell | inserter rate | against `gb-base` | inserter p50 | inserter p99 | inserter max | walker statements | walker p99 | reader p99 | stall samples, all runs | roll hold median, pooled |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
base = med([d.get("inserter_rows_per_s") or 0 for _, d in runs("gb-base")]) if runs("gb-base") else 0
for cell in BULK:
    rs = runs(cell)
    if not rs:
        continue
    ins = med([d.get("inserter_rows_per_s") or 0 for _, d in rs])
    rl = sorted(iv["hold_us"] for _, d in rs for iv in d["_rl"] if inside(iv, window(d)))
    q = lambda k, f: med([summ(d, k).get(f, 0) for _, d in rs])
    print(f"| {cell} | {ins:,.0f} rows/s | {100.0 * (ins / base - 1) if base else 0:+.1f} % | {fmt(q('inserter', 'p50_us'))} | {fmt(q('inserter', 'p99_us'))} | "
          f"{fmt(q('inserter', 'max_us'))} | {q('walker', 'n'):.0f} statements | {fmt(q('walker', 'p99_us'))} | {fmt(q('read_from_schedule', 'p99_us'))} | "
          f"{sum(len(stalls(d)) for _, d in rs)} samples | {fmt(med(rl)) if rl else '-'} |")

print("\n### 4. CZ-S1's cells read for every session, not the reader alone (from CZ-S1's archive, `relaxed`, the inserter closed-loop)")
print("\n| cell | reader p99 | reader p99.9 | stall samples, 5 runs | inserter rate | inserter p99 | inserter max | walker statements a window | walker p50 | walker p99 |")
print("|---|---|---|---|---|---|---|---|---|---|")
for cell in ("isolated", "levels", "c1", "c2", "c1c2"):
    ds = [json.load(open(p)) for p in sorted(glob.glob(f"{S1}/r*/{cell}/*.json"))]
    if not ds:
        continue
    q = lambda k, f: med([summ(d, k).get(f, 0) for d in ds])
    st = sum(sum(1 for c in d["clients"] if c["role"] == "reader" for s in c["slow"] if s[1] >= STALL_NS) for d in ds)
    print(f"| {cell} | {fmt(q('read_from_schedule', 'p99_us'))} | {fmt(q('read_from_schedule', 'p99.9_us'))} | {st} samples | "
          f"{med([d.get('inserter_rows_per_s') or 0 for d in ds]):,.0f} rows/s | {fmt(q('inserter', 'p99_us'))} | {fmt(q('inserter', 'max_us'))} | "
          f"{q('walker', 'n'):.0f} statements | {fmt(q('walker', 'p50_us'))} | {fmt(q('walker', 'p99_us'))} |")

print("\n### 5. Where the fill's disturbance falls in a segment (the cells with the prewrite and an inserter)")
print("\nThe inserter's statements are of one size, so the statement in the middle of those sent between two rolls marks the half-segment point, where the next body is asked for. "
      "A long statement is one of 30 ms or more. `disturbance ends` is the end of the last long statement that began after the half mark and before the roll. Medians over the segments wholly inside a cell's windows.")
print("\n| cell | segments | half mark to roll | segments with a long statement after the half mark | first long statement after the half mark | disturbance ends after the half mark | roll hold | sync-gate share of the holds | body hit / waited / inline |")
print("|---|---|---|---|---|---|---|---|---|")


def segments(d):
    ins = [c for c in d["clients"] if c["role"] == "inserter"]
    if not ins:
        return []
    sends = sorted((x[0], x[1]) for x in ins[0]["slow"])
    rolls = sorted(d["_rl"], key=lambda iv: iv["start_ns"])
    out = []
    for a, b in zip(rolls, rolls[1:]):
        mine = [x for x in sends if a["end_ns"] <= x[0] < b["start_ns"]]
        if len(mine) < 8:
            continue
        half = mine[len(mine) // 2][0]
        long_ = [x for x in mine if x[0] >= half and x[1] >= 30_000_000]
        out.append({"to_roll": (b["start_ns"] - half) / 1e3,
                    "first": (long_[0][0] - half) / 1e3 if long_ else None,
                    "ends": (max(x[0] + x[1] for x in long_) - half) / 1e3 if long_ else None,
                    "roll": b})
    return out


def load_from(base, cell, r):
    hits = glob.glob(f"{base}/r{r}/{cell}/*.json")
    if not hits:
        return None
    d = json.load(open(hits[0]))
    d["_rl"] = []
    for line in open(hits[0][:-5] + ".s.log"):
        if "[wal] roll" in line:
            kv = {k: int(v) for k, v in KV.findall(line)}
            m = re.search(r"body=(\w+)", line)
            kv["_body"] = m.group(1) if m else None
            d["_rl"].append(kv)
    return d


for name, base, cell in (("gb-base", f"{HERE}/out", "gb-base"), ("c1h", f"{HERE}/out", "c1h"), ("c1x", f"{HERE}/out", "c1x"),
                         ("CZ-S1's c1", S1, "c1"), ("CZ-S1's levels", S1, "levels"),
                         ("gb-c2 (no prewrite)", f"{HERE}/out", "gb-c2"), ("c1c2x (no prewrite)", f"{HERE}/out", "c1c2x")):
    segs = [sg for r in range(1, 6) if (d := load_from(base, cell, r)) for sg in segments(d)]
    if not segs:
        continue
    withl = [sg for sg in segs if sg["first"] is not None]
    hold = sum(sg["roll"]["hold_us"] for sg in segs)
    gate = 100.0 * sum(sg["roll"]["sync_gate_us"] for sg in segs) / max(1, hold)
    body = "/".join(str(sum(sg["roll"]["_body"] == k for sg in segs)) for k in ("hit", "waited", "inline"))
    print(f"| {name} | {len(segs)} segments | {fmt(med([sg['to_roll'] for sg in segs]))} | {len(withl)} of {len(segs)} | "
          f"{fmt(med([sg['first'] for sg in withl])) if withl else '-'} | {fmt(med([sg['ends'] for sg in withl])) if withl else '-'} | "
          f"{fmt(med([sg['roll']['hold_us'] for sg in segs]))} | {gate:.0f} % | {body} |")

print("\n### 6. The held inserter: how far it fell behind, and how much of core 0 it took")
print("\nA statement's due instant is rebuilt from the first send and the interval; `late` is a send 5 ms or more after it. `service time` is the inserter's summed statement time in the window.")
print("\n| cell | run | statements kept | sent late | furthest behind | inserter service time a window | walker statements | marked |")
print("|---|---|---|---|---|---|---|---|")
for cell, rate in (("c1h", 120), ("c1c2h", 120), ("c1x", 150), ("c1c2x", 150)):
    for r, d in runs(cell):
        ins = [c for c in d["clients"] if c["role"] == "inserter"][0]
        sends = sorted((x[0], x[1]) for x in ins["slow"])
        t0 = sends[0][0]
        behind = [x[0] - (t0 + k * 1e9 / rate) for k, x in enumerate(sends)]
        base = min(behind)
        behind = [b - base for b in behind]
        print(f"| {cell} | r{r} | {len(sends)} statements | {100.0 * sum(b >= 5e6 for b in behind) / len(sends):.0f} % | {fmt(max(behind) / 1e3)} | "
              f"{sum(x[1] for x in sends) / 1e9:.2f} s | {summ(d, 'walker').get('n', 0)} statements | {'build' if host(d) else '-'} |")
