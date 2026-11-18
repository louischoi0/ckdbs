#!/usr/bin/env python3
"""CZ-S0's tables (workorder-cz-checkpoint-roll-wait.md §5, M1-M4), from
out/r<run>/<cell>/*.json and *.s.log beside this file (tools/bl_mixed.py,
unmodified; cz-s0.sh names the cells).

Everything timed against the window is on CLOCK_MONOTONIC: the driver's
window edges and slow stamps, and the server's `start_ns`/`end_ns` on its
`[checkpoint] run:` and `[wal] roll` lines.

A "slow sample" is a reader sample of 1 ms or more from its scheduled
instant, which the driver stamps; a "stall sample" is one of 10 ms or more.

A "window delta" (the polls and the latch waits) is the difference between
the driver's two control reads, which fall a little after the window's two
edges (4-337 ms and 41-1,255 ms in this run), not at them.
"""
import glob
import json
import math
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
CELLS = ["isolated", "iso0", "levels", "nockpt", "half"]
LOADED = ("levels", "half", "nockpt")  # the cells with a walker and an inserter
RUNS = sorted(int(os.path.basename(p)[1:]) for p in glob.glob(f"{HERE}/out/r*") if os.path.isdir(p))
SEG = 64 << 20          # the log's segment size, bytes
STALL_NS = 10_000_000   # a stall sample
LONG_US = 10_000        # a phase long enough to make one
EPISODE_GAP_NS = 50_000_000
EDGE_NS = 5_000_000     # how close to a run's edge counts as at it
KV = re.compile(r"(\w+)=(-?\d+)")
GATE = ("log_gate_batch_sum_us", "log_gate_run_sum_us")


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
    return sorted(s for c in readers(d) for s in c["slow"] if s[1] >= STALL_NS)


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
    another: [start, end, samples]."""
    eps = []
    for t0, ns in stalls(d):
        if eps and t0 <= eps[-1][1] + EPISODE_GAP_NS:
            eps[-1][1] = max(eps[-1][1], t0 + ns)
            eps[-1][2] += 1
        else:
            eps.append([t0, t0 + ns, 1])
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


print("### 0. The host at each cell (bench/README.md rule 4), as the driver recorded it")
print("\n`competing` counts the processes matching `cc1plus|cmake --build|ctest` at the cell's start and end; "
      "a bare `kds_tests` is not in that pattern, and the run script's gate tests for it by name. "
      "`driver wait` is the driver's own wait for a 1-minute load under 1.5, which gives up after 120 s and starts.")
print("\n| cell | run | driver wait | load at start | load at end | competing at start | competing at end |")
print("|---|---|---|---|---|---|---|")
for cell in CELLS:
    for r, d in runs(cell):
        b, a = d["host_before"], d["host_after"]
        print(f"| {cell} | r{r} | {d['wait_s']:.0f} s | {b['loadavg'].split()[0]} | {a['loadavg'].split()[0]} | "
              f"{len(b['competing'])} processes | {len(a['competing'])} processes |")

print("\n### 1. Reader schedule latency (M1, M4)")
print("\n#### 1.1 per run")
print("| cell | run | samples | p50 | p99 | p99.9 | max | slow samples (>= 1 ms) | stall samples (>= 10 ms) | stall episodes |")
print("|---|---|---|---|---|---|---|---|---|---|")
P99 = {}
for cell in CELLS:
    for r, d in runs(cell):
        p = d["read_from_schedule"]
        slow = sum(len(c["slow"]) for c in readers(d))
        P99.setdefault(cell, {})[r] = p["p99_us"]
        print(f"| {cell} | r{r} | {p['n']} samples | {fmt(p['p50_us'])} | {fmt(p['p99_us'])} | {fmt(p['p99.9_us'])} | "
              f"{fmt(p['max_us'])} | {slow} samples | {len(stalls(d))} samples | {len(episodes(d))} episodes |")

print("\n#### 1.2 median of runs, and the ratio to the isolated cell")
print("| cell | runs | p50 (median) | p99 (median) | p99 range | p99.9 (median) | p99 against `isolated`'s median | against `iso0`'s median |")
print("|---|---|---|---|---|---|---|---|")
med = {c: statistics.median(P99[c].values()) for c in P99}
for cell in CELLS:
    rs = runs(cell)
    q = lambda k: statistics.median(d["read_from_schedule"][k] for _, d in rs)
    v = P99[cell].values()
    print(f"| {cell} | {len(rs)} runs | {fmt(q('p50_us'))} | {fmt(med[cell])} | {fmt(min(v))} - {fmt(max(v))} | "
          f"{fmt(q('p99.9_us'))} | {med[cell] / med['isolated']:.2f}x | {med[cell] / med['iso0']:.2f}x |")

print("\n#### 1.3 each run against the isolated run interleaved with it (V0's pairing)")
print("| run | `isolated` p99 | `iso0` p99 | `levels` p99 | `levels` / `isolated` | `nockpt` p99 | `nockpt` / `isolated` | `nockpt` / `iso0` | `half` p99 | `half` / `isolated` |")
print("|---|---|---|---|---|---|---|---|---|---|")
for r in RUNS:
    iso, i0, lv, nk, hf = (P99[c][r] for c in ("isolated", "iso0", "levels", "nockpt", "half"))
    print(f"| r{r} | {fmt(iso)} | {fmt(i0)} | {fmt(lv)} | {lv / iso:.2f}x | {fmt(nk)} | {nk / iso:.2f}x | {nk / i0:.2f}x | {fmt(hf)} | {hf / iso:.2f}x |")

print("\n### 2. What the stall samples overlap (M1, M2)")
print("\nThe driver keeps a reader's first and last core only, and the balancer moves readers inside the window, "
      "so the core a stalled reader was on at the sample is not known. `moved` counts stall samples of readers whose two cores differ.")
print("\n| cell | run | stall samples | overlap a checkpoint run | overlap a roll hold | overlap neither | moved | sessions moved out of each read core in the window |")
print("|---|---|---|---|---|---|---|---|")
for cell in LOADED:
    for r, d in runs(cell):
        n = n_ck = n_rl = n_none = n_mv = 0
        for c in readers(d):
            for t0, ns in c["slow"]:
                if ns < STALL_NS:
                    continue
                a, b = bool(overlapping(t0, t0 + ns, d["_ck"])), bool(overlapping(t0, t0 + ns, d["_rl"]))
                n += 1
                n_ck += a
                n_rl += b
                n_none += not (a or b)
                n_mv += c["core"] != c["core_end"]
        out = ", ".join(f"core {c}: {int(d['meta_delta'].get(f'load_c{c}_moves_out', 0))}" for c in read_cores(d))
        print(f"| {cell} | r{r} | {n} samples | {n_ck} samples | {n_rl} samples | {n_none} samples | {n_mv} samples | {out} |")

print("\n#### 2.1 each stall episode against the checkpoint run it falls in (M2)")
print("\nAn episode is the stall samples whose intervals lie within 50 ms of one another; they end together, when the core runs again. "
      "The reading is from timing alone, against the one run the episode overlaps (no episode here overlaps two), tested in this order:")
print("- **begin**: the run's `start` (its begin append) took 10 ms or more, and the episode overlaps the interval from the run's start to 5 ms past the end of that `start`;")
print("- **page write**: the run's longest single page write (`page_write_max_us`) is 10 ms or more and no roll hold overlaps the episode. The rule tests the run, not the instant of the write;")
print("- **publish write**: the same with the run's longest publish-phase call (`publish_writeback_max_us`): a batch of page writes, a map write, or an inline gate inside the publish, which that phase's timer hides;")
print("- **run's end**: a roll hold overlaps and the episode ends within 5 ms of the run's end - the recycle at the anchor's commit is the run's last act;")
print("- **inside the run**: a roll hold overlaps, at neither edge - the park's flush, the end append or the inline gate;")
print("- **none**: nothing above.")
print("\n| cell | run | episode at (from the window's start) | length | samples | run's core | run at | `start` max | longest page write | longest publish write | rolls overlapped (hold) | reading |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
TALLY = {}
for cell in LOADED:
    for r, d in runs(cell):
        w = window(d)
        at = lambda t: f"{(t - w[0]) / 1e6:.0f} ms"
        for a, b, n in episodes(d):
            ck, rl = overlapping(a, b, d["_ck"]), overlapping(a, b, d["_rl"])
            assert len(ck) <= 1, "the reading is defined against one run"
            reading, cols = "none", "- | - | - | - | -"
            if ck:
                run = ck[0]
                begin_end = run["start_ns"] + run["start_max_us"] * 1000 + EDGE_NS
                if run["start_max_us"] >= LONG_US and a < begin_end and b > run["start_ns"]:
                    reading = "begin"
                elif run["page_write_max_us"] >= LONG_US and not rl:
                    reading = "page write"
                elif run["publish_writeback_max_us"] >= LONG_US and not rl:
                    reading = "publish write"
                elif rl and abs(b - run["end_ns"]) <= EDGE_NS:
                    reading = "run's end"
                elif rl:
                    reading = "inside the run"
                cols = (f"{run['core']} | {at(run['start_ns'])} - {at(run['end_ns'])} | {fmt(run['start_max_us'])} | "
                        f"{fmt(run['page_write_max_us'])} | {fmt(run['publish_writeback_max_us'])}")
            t = TALLY.setdefault(cell, {}).setdefault(reading, [0, 0])
            t[0] += 1
            t[1] += n
            holds = ", ".join(fmt(iv["hold_us"]) for iv in rl) or "none"
            print(f"| {cell} | r{r} | {at(a)} - {at(b)} | {fmt((b - a) / 1e3)} | {n} samples | {cols} | {holds} | {reading} |")

print("\n#### 2.2 the episodes by reading")
print("| cell | reading | episodes | stall samples |")
print("|---|---|---|---|")
for cell, t in TALLY.items():
    for k, (e, n) in sorted(t.items()):
        print(f"| {cell} | {k} | {e} episodes | {n} samples |")

print("\n### 3. The read cores over the window: holds and the stream latch (M1)")
print("\n`system polls > 10 ms` and the latch waits are window deltas; `longest system poll` is since the server's start. "
      "`pool` is the pages resident at the closing read, of 65,536 frames, and the misses in the window.")
print("\n| cell | run | read cores | system polls > 10 ms (sum) | longest system poll (max) | foreground polls > 10 ms (sum) | stream-latch wait, read cores (sum) | of it roll-blocked | roll-blocked waits | stream-latch wait, core 0 | checkpoint runs on read cores | pool: resident / misses |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for cell in CELLS:
    for r, d in runs(cell):
        rc = read_cores(d)
        mb, md = d["meta_by_core"], d["meta_delta"]
        s = lambda k: sum(mb[str(c)].get(k, 0) for c in rc)
        m = lambda k: sum(md.get(k.format(c), 0) for c in rc)
        print(f"| {cell} | r{r} | {','.join(map(str, rc))} | {int(s('sched_system_polls_over_10ms'))} polls | "
              f"{fmt(max(mb[str(c)].get('sched_system_longest_poll_us', 0) for c in rc))} | "
              f"{int(s('sched_foreground_polls_over_10ms'))} polls | {fmt(m('load_c{}_waited_wal_stream_us'))} | "
              f"{fmt(m('load_c{}_wal_roll_blocked_wait_us'))} | {int(m('load_c{}_wal_roll_blocked_waits'))} waits | "
              f"{fmt(md.get('load_c0_waited_wal_stream_us', 0))} | {int(s('checkpoint_runs'))} runs | "
              f"{int(d['meta_m1'].get('pool_resident', 0)):,} / {int(md.get('pool_misses', 0)):,} pages |")

print("\n### 4. The log and the rolls over the window (M1's roll-hold distribution, M3, M4)")
print("\n`live segments` is derived from the log lines, not read from the device: the segment being written at the window's end, "
      "less the segment of the lowest redo start among each core's latest `anchor published` line before it (the fold), plus one.")
print("\n| cell | run | log rate | inserter rate | rolls in window | hold median | hold p90 | hold max | hold total | share of window under a roll hold | checkpoint runs in window | share of window under a checkpoint run | live segments at the end |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for cell in CELLS:
    for r, d in runs(cell):
        w = window(d)
        rl = [iv for iv in d["_rl"] if inside(iv, w)]
        ck = [iv for iv in d["_ck"] if inside(iv, w)]
        hold = sorted(iv["hold_us"] for iv in rl)
        h = (f"{fmt(statistics.median(hold))} | {fmt(rank(hold, 0.9))} | {fmt(hold[-1])}" if hold else "- | - | -")
        print(f"| {cell} | r{r} | {(d.get('log_bytes_per_s') or 0) / 1e6:.1f} MB/s | {d.get('inserter_rows_per_s', 0):,.0f} rows/s | "
              f"{len(rl)} rolls | {h} | {fmt(sum(hold))} | {covered(rl, w):.0f} % | {len(ck)} runs | {covered(ck, w):.0f} % | "
              f"{live_segments(d, w)} segments |")

print("\n#### 4.1 roll holds pooled over the runs of a cell")
print("| cell | rolls | hold p50 | hold p90 | hold p99 | hold max | holds over 300 ms | sync-gate share of the hold |")
print("|---|---|---|---|---|---|---|---|")
for cell in LOADED:
    rl = [iv for _, d in runs(cell) for iv in d["_rl"] if inside(iv, window(d))]
    hold = sorted(iv["hold_us"] for iv in rl)
    gate = 100.0 * sum(iv["sync_gate_us"] for iv in rl) / sum(hold)
    print(f"| {cell} | {len(rl)} rolls | {fmt(statistics.median(hold))} | {fmt(rank(hold, 0.9))} | {fmt(rank(hold, 0.99))} | "
          f"{fmt(hold[-1])} | {sum(x > 300_000 for x in hold)} rolls | {gate:.1f} % |")

print("\n### 5. Each checkpoint run of the default arm against the rolls it overlaps (M2)")
print("\n`on-reactor, timed` is the run line's `start`, `log_gate_*`, `page_write`, `map_write` and `publish_writeback` sums: "
      "the phases the line times on the reactor thread. `parked` is `log_park_us + data_park_us`. "
      "`rest` is the wall time less both: yields, and whatever the line does not time - the park's flush, the end append and the recycle among it.")
print("\n| run | core | wall | pages | steps | gate parks | start (begin append) max | inline gate n / sum | log park | data park | on-reactor, timed | rest | rolls overlapped | overlap with roll holds | longest roll overlapped |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for r, d in runs("levels"):
    for iv in d["_ck"]:
        if not inside(iv, window(d)):
            continue
        ov = overlapping(iv["start_ns"], iv["end_ns"], d["_rl"])
        ovl = sum(min(x["end_ns"], iv["end_ns"]) - max(x["start_ns"], iv["start_ns"]) for x in ov) / 1e3
        timed = sum(iv[k] for k in ("start_sum_us", *GATE, "page_write_sum_us", "map_write_sum_us", "publish_writeback_sum_us"))
        parked = iv["log_park_us"] + iv["data_park_us"]
        print(f"| r{r} | {iv['core']} | {fmt(iv['wall_us'])} | {iv['pages']} pages | {iv['steps']} steps | {iv['gate_parks']} parks | "
              f"{fmt(iv['start_max_us'])} | {iv['log_gate_batch_n'] + iv['log_gate_run_n']} / {fmt(sum(iv[k] for k in GATE))} | "
              f"{fmt(iv['log_park_us'])} | {fmt(iv['data_park_us'])} | {fmt(timed)} | {fmt(max(0, iv['wall_us'] - parked - timed))} | "
              f"{len(ov)} rolls | {fmt(ovl)} | {fmt(max((x['hold_us'] for x in ov), default=0))} |")

print("\n#### 5.1 a read core's stream-latch wait against the latch sites its runs time")
print("\nThe begin append is inside `start` and the inline gate is `log_gate_*`, so their sums bound the latch wait those two sites took; "
      "what is left of the core's stream-latch wait was taken at a site the run line does not time, or by another taker. "
      "An inline gate taken inside the publish is hidden in `publish_writeback`, so the last column takes that phase out too.")
print("\n| cell | run | core | stream-latch wait in window | roll-blocked part | checkpoint runs overlapping the window | their `start` sum | their inline-gate sum | not at a timed latch site (at least) | the same, less their publish writeback |")
print("|---|---|---|---|---|---|---|---|---|---|")
TOTAL = {}
for cell in LOADED:
    for r, d in runs(cell):
        w, md = window(d), d["meta_delta"]
        for c in read_cores(d):
            lw = md.get(f"load_c{c}_waited_wal_stream_us", 0)
            rb = md.get(f"load_c{c}_wal_roll_blocked_wait_us", 0)
            mine = [iv for iv in d["_ck"] if iv["core"] == c and inside(iv, w)]
            st = sum(iv["start_sum_us"] for iv in mine)
            gt = sum(iv[k] for iv in mine for k in GATE)
            pb = sum(iv["publish_writeback_sum_us"] for iv in mine)
            left, left2 = max(0, lw - st - gt), max(0, lw - st - gt - pb)
            t = TOTAL.setdefault(cell, [0] * 6)
            for i, v in enumerate((lw, rb, st, gt, left, left2)):
                t[i] += v
            if lw >= 1000 or mine:
                print(f"| {cell} | r{r} | {c} | {fmt(lw)} | {fmt(rb)} | {len(mine)} runs | {fmt(st)} | {fmt(gt)} | {fmt(left)} | {fmt(left2)} |")

print("\n#### 5.2 the same, summed over the read cores and the runs of a cell (the last two columns core by core)")
print("| cell | stream-latch wait, read cores | roll-blocked part | `start` sum of their checkpoint runs | their inline-gate sum | not at a timed latch site (at least) | the same, less their publish writeback |")
print("|---|---|---|---|---|---|---|")
for cell, t in TOTAL.items():
    print(f"| {cell} | " + " | ".join(fmt(v) for v in t) + " |")
