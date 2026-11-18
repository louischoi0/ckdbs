#!/usr/bin/env python3
"""CZ-S1's tables (workorder-cz-checkpoint-roll-wait.md §5, the ceiling), from
out/r<run>/<cell>/*.json and *.s.log beside this file (tools/bl_mixed.py with
the spike's `--stall-cores`; cz-s1.sh names the cells). The loaders and the
episode rule are copied from CZ-S0's `cz_s0.py`, so that this archive stands
alone.

Everything timed against the window is on CLOCK_MONOTONIC: the driver's
window edges and slow stamps, and the server's `start_ns`/`end_ns` on its
`[checkpoint] run:` and `[wal] roll` lines.

A "slow sample" is a reader sample of 1 ms or more from its scheduled
instant, which the driver stamps; a "stall sample" is one of 10 ms or more,
and carries the core a SHOW META sent right after it was answered on.

A "window delta" (the polls and the latch waits) is the difference between
the driver's two control reads, which fall a little after the window's two
edges, not at them.

A "site" is one of the cadence checkpoint's stream-latch sites, counted by
the spike on the run line (`cz_<site>_*`): `calls` are calls made, tries
included; `busy` the tries that found the latch held (or another thread's
flush in flight); `parks` those of them parked on a roll, the others one yield
and a retry; `bound` the times 16 parks were spent and the blocking call made; `call` the reactor time inside the calls; `latch` the part of it spent
waiting for the stream latch; `park` the wall time the task was parked for a
busy latch, the reactor running other work.
"""
import glob
import json
import math
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
CELLS = ["isolated", "levels", "c1", "c2", "c1c2"]
LOADED = ("levels", "c1", "c2", "c1c2")  # the cells with a walker and an inserter
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


SITES = ("begin", "assert", "flush", "end", "recycle", "gate_step", "gate_publish")


def site(iv, s, f):
    return iv.get(f"cz_{s}_{f}", 0)


def in_window_runs(d, cores=None):
    w = window(d)
    return [iv for iv in d["_ck"] if inside(iv, w) and (cores is None or iv["core"] in cores)]


def cores_of(cs):
    seen = {}
    for c in cs:
        seen[c] = seen.get(c, 0) + 1
    return ", ".join(f"core {c}: {n}" if c is not None else f"unread: {n}" for c, n in sorted(seen.items(), key=lambda kv: (kv[0] is None, kv[0])))


print("### 0. The host at each cell (bench/README.md rule 4), as the driver recorded it")
print("\n`competing` counts the processes matching `cc1plus|cmake --build|ctest` at the cell's start and end; "
      "a bare `kds_tests` is not in that pattern, and the run script's gate tests for it by name. "
      "`driver wait` is the driver's own wait for a 1-minute load under 1.5, which gives up after 120 s and starts.")
print("\n| cell | run | driver wait | load at start | load at end | competing at start | competing at end | `cz_mode` on its run lines |")
print("|---|---|---|---|---|---|---|---|")
for cell in CELLS:
    for r, d in runs(cell):
        b, a = d["host_before"], d["host_after"]
        modes = sorted({iv.get("cz_mode") for iv in d["_ck"]})
        print(f"| {cell} | r{r} | {d['wait_s']:.0f} s | {b['loadavg'].split()[0]} | {a['loadavg'].split()[0]} | "
              f"{len(b['competing'])} processes | {len(a['competing'])} processes | {modes} |")

print("\n### 1. Reader schedule latency")
print("\n#### 1.1 per run")
print("| cell | run | samples | p50 | p99 | p99.9 | max | slow samples (>= 1 ms) | stall samples (>= 10 ms) | stall episodes | errors |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
P99, P999, NSTALL = {}, {}, {}
for cell in CELLS:
    for r, d in runs(cell):
        p = d["read_from_schedule"]
        slow = sum(len(c["slow"]) for c in readers(d))
        P99.setdefault(cell, {})[r] = p["p99_us"]
        P999.setdefault(cell, {})[r] = p["p99.9_us"]
        NSTALL.setdefault(cell, {})[r] = len(stalls(d))
        print(f"| {cell} | r{r} | {p['n']} samples | {fmt(p['p50_us'])} | {fmt(p['p99_us'])} | {fmt(p['p99.9_us'])} | "
              f"{fmt(p['max_us'])} | {slow} samples | {len(stalls(d))} samples | {len(episodes(d))} episodes | {d['errors']} |")

print("\n#### 1.2 median of runs, and the ratio to the isolated cell (CZ-Q1's bar; p99.9 and the stall count beside it, CZ-Q11)")
print("| cell | runs | p50 (median) | p99 (median) | p99 range | p99 against `isolated`'s median | pairs within 2x | p99.9 (median) | p99.9 range | stall samples, all runs | stall samples a run |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
med = {c: statistics.median(P99[c].values()) for c in P99}
for cell in CELLS:
    rs = runs(cell)
    q = lambda k: statistics.median(d["read_from_schedule"][k] for _, d in rs)
    v, v9 = P99[cell].values(), P999[cell].values()
    pairs = [r for r in P99[cell] if r in P99["isolated"]]
    within = sum(P99[cell][r] <= 2 * P99["isolated"][r] for r in pairs)
    ns = [NSTALL[cell][r] for r in sorted(NSTALL[cell])]
    print(f"| {cell} | {len(rs)} runs | {fmt(q('p50_us'))} | {fmt(med[cell])} | {fmt(min(v))} - {fmt(max(v))} | "
          f"{med[cell] / med['isolated']:.2f}x | {within} of {len(pairs)} | {fmt(statistics.median(v9))} | {fmt(min(v9))} - {fmt(max(v9))} | "
          f"{sum(ns)} samples | {', '.join(map(str, ns))} |")

print("\n#### 1.3 each run against the isolated run interleaved with it")
print("| run | `isolated` p99 | " + " | ".join(f"`{c}` p99 | `{c}` / `isolated`" for c in LOADED) + " |")
print("|---|---|" + "---|---|" * len(LOADED))
for r in RUNS:
    if r not in P99.get("isolated", {}):
        continue
    iso = P99["isolated"][r]
    cols = " | ".join(f"{fmt(P99[c][r])} | {P99[c][r] / iso:.2f}x" if r in P99.get(c, {}) else "- | -" for c in LOADED)
    print(f"| r{r} | {fmt(iso)} | {cols} |")

print("\n#### 1.4 the same against the isolated cells no other session's build reached")
CLEAN = [r for r, d in runs("isolated")
         if not d["host_before"]["competing"] and not d["host_after"]["competing"]
         and float(d["host_before"]["loadavg"].split()[0]) < 2]
clean_med = statistics.median(P99["isolated"][r] for r in CLEAN)
print(f"\nAn isolated cell is taken as clean when no `cc1plus|cmake --build|ctest` process was seen at either end and the load at its start was under 2: "
      f"{', '.join(f'r{r}' for r in CLEAN)}, whose p99 median is {fmt(clean_med)}. A loaded cell is marked `build` when such a process was seen at either of its ends.")
print("\n| cell | p99 (median) | against the clean isolated median | runs within 2x of it | each run's p99 (ratio; `build` where one was seen) |")
print("|---|---|---|---|---|")
for cell in LOADED:
    rows = []
    for r, d in runs(cell):
        hit = bool(d["host_before"]["competing"] or d["host_after"]["competing"])
        rows.append(f"r{r} {fmt(P99[cell][r])} ({P99[cell][r] / clean_med:.2f}x{', build' if hit else ''})")
    within = sum(P99[cell][r] <= 2 * clean_med for r in P99[cell])
    print(f"| {cell} | {fmt(med[cell])} | {med[cell] / clean_med:.2f}x | {within} of {len(P99[cell])} | {'; '.join(rows)} |")

print("\n### 2. What the stall samples overlap, and the cores they were answered on")
print("\n`cores` is the core a SHOW META sent right after each stall sample was answered on (`--stall-cores`): the session's core "
      "at that head, which the placer may have moved since the sample.")
print("\n| cell | run | stall samples | overlap a checkpoint run | overlap a roll hold | overlap neither | cores | sessions moved out of each read core in the window |")
print("|---|---|---|---|---|---|---|---|")
for cell in LOADED:
    for r, d in runs(cell):
        n = n_ck = n_rl = n_none = 0
        cs = []
        for t0, ns, core in stalls(d):
            a, b = bool(overlapping(t0, t0 + ns, d["_ck"])), bool(overlapping(t0, t0 + ns, d["_rl"]))
            n += 1
            n_ck += a
            n_rl += b
            n_none += not (a or b)
            cs.append(core)
        out = ", ".join(f"core {c}: {int(d['meta_delta'].get(f'load_c{c}_moves_out', 0))}" for c in read_cores(d))
        print(f"| {cell} | r{r} | {n} samples | {n_ck} samples | {n_rl} samples | {n_none} samples | {cores_of(cs) or '-'} | {out} |")

print("\n#### 2.1 each stall episode against the checkpoint runs it overlaps")
print("\nAn episode is the stall samples whose intervals lie within 50 ms of one another. The reading is CZ-S0's rule, from timing, "
      "against the overlapped run on the episode's most frequent core when there is one, else the first overlapped run, tested in this order: "
      "**begin** (the run's `start` took 10 ms or more and the episode overlaps it, to 5 ms past its end); "
      "**page write** (the run's longest page write is 10 ms or more and no roll hold overlaps); "
      "**publish write** (the same with its longest publish-phase call); "
      "**run's end** (a roll hold overlaps and the episode ends within 5 ms of the run's end); "
      "**inside the run** (a roll hold overlaps, at neither edge); **none**. "
      "`latch at sites` is that run's stream-latch wait summed over the spike's sites, and `longest site call` the longest single call with its site.")
print("\n| cell | run | episode at (from the window's start) | length | samples | samples' cores | runs overlapped (cores) | run read | run at | `start` max | longest page write | longest publish write | latch at sites | longest site call | rolls overlapped (hold) | reading |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
TALLY = {}
for cell in LOADED:
    for r, d in runs(cell):
        w = window(d)
        at = lambda t: f"{(t - w[0]) / 1e6:.0f} ms"
        for a, b, n, cs in episodes(d):
            ck, rl = overlapping(a, b, d["_ck"]), overlapping(a, b, d["_rl"])
            reading, cols = "none", "- | - | - | - | - | - | -"
            if ck:
                known = [c for c in cs if c is not None]
                top = max(set(known), key=known.count) if known else None
                run = next((iv for iv in ck if iv["core"] == top), ck[0])
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
                longest = max(SITES, key=lambda s: site(run, s, "call_max_us"))
                cols = (f"{run['core']} | {at(run['start_ns'])} - {at(run['end_ns'])} | {fmt(run['start_max_us'])} | "
                        f"{fmt(run['page_write_max_us'])} | {fmt(run['publish_writeback_max_us'])} | "
                        f"{fmt(sum(site(run, s, 'latch_us') for s in SITES))} | {fmt(site(run, longest, 'call_max_us'))} ({longest})")
            t = TALLY.setdefault(cell, {}).setdefault(reading, [0, 0])
            t[0] += 1
            t[1] += n
            holds = ", ".join(fmt(iv["hold_us"]) for iv in rl) or "none"
            print(f"| {cell} | r{r} | {at(a)} - {at(b)} | {fmt((b - a) / 1e3)} | {n} samples | {cores_of(cs)} | "
                  f"{', '.join(str(iv['core']) for iv in ck) or 'none'} | {cols} | {holds} | {reading} |")

print("\n#### 2.2 the episodes by reading")
print("| cell | reading | episodes | stall samples |")
print("|---|---|---|---|")
for cell, t in TALLY.items():
    for k, (e, n) in sorted(t.items()):
        print(f"| {cell} | {k} | {e} episodes | {n} samples |")

print("\n### 3. The read cores over the window: holds and the stream latch")
print("\n`system polls > 10 ms` and the latch waits are window deltas; `longest system poll` is since the server's start.")
print("\n| cell | run | read cores | system polls > 10 ms (sum) | longest system poll (max) | foreground polls > 10 ms (sum) | stream-latch wait, read cores (sum) | of it roll-blocked | roll-blocked waits | stream-latch wait, core 0 | checkpoint runs on read cores |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
LATCH = {}
for cell in CELLS:
    for r, d in runs(cell):
        rc = read_cores(d)
        mb, md = d["meta_by_core"], d["meta_delta"]
        s = lambda k: sum(mb[str(c)].get(k, 0) for c in rc)
        m = lambda k: sum(md.get(k.format(c), 0) for c in rc)
        LATCH.setdefault(cell, []).append(m('load_c{}_waited_wal_stream_us'))
        print(f"| {cell} | r{r} | {','.join(map(str, rc))} | {int(s('sched_system_polls_over_10ms'))} polls | "
              f"{fmt(max(mb[str(c)].get('sched_system_longest_poll_us', 0) for c in rc))} | "
              f"{int(s('sched_foreground_polls_over_10ms'))} polls | {fmt(m('load_c{}_waited_wal_stream_us'))} | "
              f"{fmt(m('load_c{}_wal_roll_blocked_wait_us'))} | {int(m('load_c{}_wal_roll_blocked_waits'))} waits | "
              f"{fmt(md.get('load_c0_waited_wal_stream_us', 0))} | {int(s('checkpoint_runs'))} runs |")

print("\n#### 3.1 the read cores' stream-latch wait, a cell")
print("| cell | sum over the runs | a run, from the least |")
print("|---|---|---|")
for cell in CELLS:
    print(f"| {cell} | {fmt(sum(LATCH[cell]))} | {', '.join(fmt(x) for x in sorted(LATCH[cell]))} |")

print("\n#### 3.2 the read cores' holds, by the scheduler's own counters (independent of the latch counters)")
print("\nA tried latch charges no wait, so under `c1` the latch counters of §3 and §5 read zero at a tried site by construction. "
      "These do not: `in-run holds` are a reactor's unpolled stretches inside a checkpoint run of its core, `foreground polls` its foreground tasks' polls. "
      "Counts are window deltas, per read core in core order; a `longest` is since the server's start. `checkpoint pages` is the engine's count of pages the checkpoint wrote "
      "in the window against the pages of the runs that logged a line and overlap it: a run in flight at the server's stop is abandoned and logs none.")
print("\n| cell | run | in-run holds > 10 ms | in-run holds > 100 ms | longest in-run hold | system polls > 100 ms | foreground polls > 10 ms | longest foreground poll | page-latch spin turns | checkpoint pages: engine / logged runs |")
print("|---|---|---|---|---|---|---|---|---|---|")
HOLD = {}
for cell in CELLS:
    for r, d in runs(cell):
        rc = read_cores(d)
        mb, md = d["meta_by_core"], d["meta_delta"]
        g = lambda k: [int(mb[str(c)].get(k, 0)) for c in rc]
        j = lambda xs: ", ".join(map(str, xs))
        logged = sum(iv["pages"] for iv in in_window_runs(d))
        t = HOLD.setdefault(cell, [0, 0, 0])
        t[0] += sum(g("sched_hold_in_run_over_10ms"))
        t[1] += sum(g("sched_hold_in_run_over_100ms"))
        t[2] += sum(g("sched_foreground_polls_over_10ms"))
        print(f"| {cell} | r{r} | {j(g('sched_hold_in_run_over_10ms'))} holds | {j(g('sched_hold_in_run_over_100ms'))} holds | "
              f"{fmt(max(g('sched_hold_in_run_longest_us')))} | {j(g('sched_system_polls_over_100ms'))} polls | "
              f"{j(g('sched_foreground_polls_over_10ms'))} polls | {fmt(max(g('sched_foreground_longest_poll_us')))} | "
              f"{int(md.get('contention_page_latch_spin_turns', 0)):,} turns | "
              f"{int(md.get('contention_checkpoint_pages_written', 0)):,} / {logged:,} pages |")

print("\n#### 3.3 the same, a cell")
print("| cell | in-run holds > 10 ms, read cores, 5 runs | in-run holds > 100 ms | foreground polls > 10 ms |")
print("|---|---|---|---|")
for cell in CELLS:
    t = HOLD[cell]
    print(f"| {cell} | {t[0]} holds | {t[1]} holds | {t[2]} polls |")

print("\n### 4. The log, the rolls and the inserter over the window (C2's subject; CZ-Q2's guards, reported)")
print("\n`live segments` is derived from the log lines, as in CZ-S0. `body` is the roll's body outcome: `hit` the next segment's body "
      "was ready, `waited` the roll waited for the preparer, `inline` the roll built it.")
print("\n| cell | run | log rate | inserter rate | rolls in window | hold median | hold p90 | hold max | hold total | share of window under a roll hold | body hit / waited / inline | checkpoint runs in window | share of window under a checkpoint run | longest run | live segments at the end |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
INS = {}
for cell in CELLS:
    for r, d in runs(cell):
        w = window(d)
        rl = [iv for iv in d["_rl"] if inside(iv, w)]
        ck = [iv for iv in d["_ck"] if inside(iv, w)]
        hold = sorted(iv["hold_us"] for iv in rl)
        h = (f"{fmt(statistics.median(hold))} | {fmt(rank(hold, 0.9))} | {fmt(hold[-1])}" if hold else "- | - | -")
        INS.setdefault(cell, []).append(d.get("inserter_rows_per_s", 0))
        body = "/".join(str(sum(iv.get("_body") == k for iv in rl)) for k in ("hit", "waited", "inline"))
        print(f"| {cell} | r{r} | {(d.get('log_bytes_per_s') or 0) / 1e6:.1f} MB/s | {d.get('inserter_rows_per_s', 0):,.0f} rows/s | "
              f"{len(rl)} rolls | {h} | {fmt(sum(hold))} | {covered(rl, w):.0f} % | {body} | {len(ck)} runs | {covered(ck, w):.0f} % | "
              f"{fmt(max((iv['wall_us'] for iv in ck), default=0))} | {live_segments(d, w)} segments |")

print("\n#### 4.1 roll holds pooled over the runs of a cell, and the inserter")
print("| cell | rolls | hold p50 | hold p90 | hold p99 | hold max | holds over 300 ms | sync-gate share of the hold | create share of the hold | inserter rate (median) | against `levels` |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for cell in LOADED:
    rl = [iv for _, d in runs(cell) for iv in d["_rl"] if inside(iv, window(d))]
    hold = sorted(iv["hold_us"] for iv in rl)
    gate = 100.0 * sum(iv["sync_gate_us"] for iv in rl) / sum(hold)
    create = 100.0 * sum(iv["create_us"] for iv in rl) / sum(hold)
    ins, base = statistics.median(INS[cell]), statistics.median(INS["levels"])
    print(f"| {cell} | {len(rl)} rolls | {fmt(statistics.median(hold))} | {fmt(rank(hold, 0.9))} | {fmt(rank(hold, 0.99))} | "
          f"{fmt(hold[-1])} | {sum(x > 300_000 for x in hold)} rolls | {gate:.1f} % | {create:.1f} % | {ins:,.0f} rows/s | {100.0 * (ins / base - 1):+.1f} % |")

print("\n### 5. The cadence checkpoint's stream-latch sites, as the spike counted them")
print("\nSummed over the checkpoint runs that overlap the window, all 5 runs of a cell; `longest call` and `longest latch wait` are the largest single ones.")
for who, pick in (("read cores (1-3)", lambda iv: iv["core"] != 0), ("core 0", lambda iv: iv["core"] == 0)):
    print(f"\n#### 5.{1 if 'read' in who else 2} {who}")
    print("| cell | checkpoint runs | site | calls | busy | parked on a roll | bound reached | reactor time in calls | longest call | stream-latch wait | longest latch wait | parked for the latch |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for cell in LOADED:
        ivs = [iv for _, d in runs(cell) for iv in in_window_runs(d) if pick(iv)]
        for s in SITES:
            t = lambda f: sum(site(iv, s, f) for iv in ivs)
            mx = lambda f: max((site(iv, s, f) for iv in ivs), default=0)
            print(f"| {cell} | {len(ivs)} runs | {s} | {t('calls')} | {t('busy')} | {t('parks')} | {t('bound')} | {fmt(t('call_us'))} | "
                  f"{fmt(mx('call_max_us'))} | {fmt(t('latch_us'))} | {fmt(mx('latch_max_us'))} | {fmt(t('park_us'))} |")

print("\n#### 5.3 the read cores' stream-latch wait against what the sites counted")
print("\nThe first column is the engine's own counter (a window delta); the second is the spike's, over the read cores' runs that overlap the window. "
      "They differ by the runs that straddle a window edge or a control read, and by any taker that is not a site.")
print("\n| cell | stream-latch wait, read cores (engine's counter) | at the spike's sites | " + " | ".join(SITES) + " |")
print("|---|---|---|" + "---|" * len(SITES))
for cell in LOADED:
    ivs = [iv for _, d in runs(cell) for iv in in_window_runs(d) if iv["core"] != 0]
    per = [sum(site(iv, s, "latch_us") for iv in ivs) for s in SITES]
    print(f"| {cell} | {fmt(sum(LATCH[cell]))} | {fmt(sum(per))} | " + " | ".join(fmt(x) for x in per) + " |")

print("\n#### 5.4 a read core's checkpoint run: what held its reactor longest (CZ-Q12: the page writes C1 leaves)")
print("\nOver the read cores' runs that overlap the window: the longest single call of each kind in any run of the cell, and the runs in which it reached 10 ms.")
print("\n| cell | runs | `start` (begin append) | page write | publish-phase call | step's whole hold | flush call | end append | recycle | inline gate (step / publish) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for cell in LOADED:
    ivs = [iv for _, d in runs(cell) for iv in in_window_runs(d) if iv["core"] != 0]
    def col(get):
        xs = [get(iv) for iv in ivs]
        return f"{fmt(max(xs, default=0))} ({sum(x >= LONG_US for x in xs)} runs)"
    print(f"| {cell} | {len(ivs)} runs | {col(lambda iv: iv['start_max_us'])} | {col(lambda iv: iv['page_write_max_us'])} | "
          f"{col(lambda iv: iv['publish_writeback_max_us'])} | {col(lambda iv: iv['step_max_us'])} | "
          f"{col(lambda iv: site(iv, 'flush', 'call_max_us'))} | {col(lambda iv: site(iv, 'end', 'call_max_us'))} | "
          f"{col(lambda iv: site(iv, 'recycle', 'call_max_us'))} | "
          f"{col(lambda iv: site(iv, 'gate_step', 'call_max_us'))} / {col(lambda iv: site(iv, 'gate_publish', 'call_max_us'))} |")

print("\n#### 5.5 checkpoint runs: wall time and parks (CZ-Q2's guard, reported)")
print("| cell | runs in window (all cores) | wall median | wall max | log park, sum | data park, sum | parked for the latch, sum | gate parks | recycles skipped on a busy latch |")
print("|---|---|---|---|---|---|---|---|---|")
for cell in LOADED:
    ivs = [iv for _, d in runs(cell) for iv in in_window_runs(d)]
    wall = sorted(iv["wall_us"] for iv in ivs)
    lp = sum(site(iv, s, "park_us") for iv in ivs for s in SITES)
    print(f"| {cell} | {len(ivs)} runs | {fmt(statistics.median(wall))} | {fmt(wall[-1])} | {fmt(sum(iv['log_park_us'] for iv in ivs))} | "
          f"{fmt(sum(iv['data_park_us'] for iv in ivs))} | {fmt(lp)} | {sum(iv['gate_parks'] for iv in ivs)} parks | "
          f"{sum(site(iv, 'recycle', 'busy') for iv in ivs)} |")

print("\n### 6. The reader samples of 1 ms to 10 ms: inside and outside a read core's checkpoint run (what C1 leaves)")
print("\nA sample is inside when its interval overlaps a checkpoint run of a read core (of any core in `isolated`). The rate is samples per second of the window so covered.")
print("\n| cell | samples of 1-10 ms, 5 runs | inside a run | window under such runs | rate inside | rate outside |")
print("|---|---|---|---|---|---|")
for cell in CELLS:
    n_in = n_out = 0
    t_in = t_out = 0.0
    for r, d in runs(cell):
        w = window(d)
        ivs = [iv for iv in d["_ck"] if iv["core"] != 0 or cell == "isolated"]
        slow = [s for c in readers(d) for s in c["slow"] if 1_000_000 <= s[1] < STALL_NS]
        k = sum(bool(overlapping(s[0], s[0] + s[1], ivs)) for s in slow)
        c = covered(ivs, w) / 100.0 * (w[1] - w[0]) / 1e9
        n_in += k
        n_out += len(slow) - k
        t_in += c
        t_out += (w[1] - w[0]) / 1e9 - c
    print(f"| {cell} | {n_in + n_out} samples | {n_in} samples | {t_in:.1f} s of {t_in + t_out:.0f} s | "
          f"{n_in / t_in if t_in else 0:.1f} /s | {n_out / t_out:.1f} /s |")
