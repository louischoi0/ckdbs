#!/usr/bin/env python3
"""BL rung-3 probe tables, from mixed/r<run>/*.json beside this file
(tools/bl_mixed.py at 67973ebc): reader latency per arm, where the `normal`
walker sat against the readers, and each core's holds and roll waits."""
import glob
import json
import os
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = ["p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "p99.9_us", "max_us"]
# arm -> the tag's distinguishing middle (bl_mixed.py's run tag).
ARMS = [("isolated", "-isolated-relaxed-w0-slicedflt-levels-r"),
        ("nwonly", "-isolated-relaxed-w0-slicedflt-levels-nw1-r"),
        ("levels", "-mixed-relaxed-w0-slicedflt-levels-r"),
        ("probe", "-mixed-relaxed-w0-slicedflt-levels-nw1-r")]
RUNS = sorted(int(os.path.basename(p)[1:]) for p in glob.glob(f"{HERE}/mixed/r*") if os.path.isdir(p))


def fmt(us):
    return f"{us / 1e3:.1f} ms" if us >= 1e3 else f"{us:.0f} µs"


def load(arm, r):
    pat = dict(ARMS)[arm]
    hits = [f for f in glob.glob(f"{HERE}/mixed/r{r}/bl-mixed-k4-kernel-*.json") if pat in f]
    return json.load(open(hits[0])) if hits else None


def runs(arm):
    return [(r, d) for r in RUNS if (d := load(arm, r))]


for kind, key in (("schedule", "read_from_schedule"), ("service", "read_service")):
    print(f"\n#### reader {kind} latency, per run")
    print("| arm | run | n | " + " | ".join(k[:-3] for k in KEYS) + " |")
    print("|---|---|---|" + "---|" * len(KEYS))
    for arm, _ in ARMS:
        for r, d in runs(arm):
            p = d[key]
            print(f"| {arm} | r{r} | {p['n']} | " + " | ".join(fmt(p[k]) for k in KEYS) + " |")
    print(f"\n#### reader {kind} latency, median of runs")
    print("| arm | runs | " + " | ".join(k[:-3] for k in KEYS) + " |")
    print("|---|---|" + "---|" * len(KEYS))
    for arm, _ in ARMS:
        ps = [d[key] for _, d in runs(arm)]
        if ps:
            print(f"| {arm} | {len(ps)} | " + " | ".join(fmt(statistics.median(p[k] for p in ps)) for k in KEYS) + " |")

# Where the excess sits: each reader's samples at or above 1 ms (the driver's
# slow stamps, from the schedule), split by whether the reader ended on the
# core the `normal` walker ended on.
print("\n#### the `normal` walker against the readers (arms with one)")
print("| arm | run | nwalker core (start->end) | readers sharing it at end | readers moved | "
      "slow samples (>=1 ms) on shared readers | on the others | slow samples per shared reader | per other reader | nwalker p50 / p99 | nwalker stmts |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for arm in ("nwonly", "probe"):
    for r, d in runs(arm):
        cl = d["clients"]
        nw = next(c for c in cl if c["role"] == "nwalker")
        rd = [c for c in cl if c["role"] == "reader"]
        shared = [c for c in rd if c["core_end"] == nw["core_end"]]
        other = [c for c in rd if c["core_end"] != nw["core_end"]]
        s_sh = sum(len(c["slow"]) for c in shared)
        s_ot = sum(len(c["slow"]) for c in other)
        p = d["nwalker"]
        print(f"| {arm} | r{r} | {int(nw['core'])}->{int(nw['core_end'])} | {len(shared)} | {d['readers_moved']} | "
              f"{s_sh} | {s_ot} | {s_sh / len(shared) if shared else 0:.1f} | {s_ot / len(other) if other else 0:.1f} | "
              f"{fmt(p['p50_us'])} / {fmt(p['p99_us'])} | {p['n']:,} |")

# Each interactive core: its longest foreground poll (a statement or a walk
# slice, since start), polls over 10 ms in the window, its longest system poll
# (the checkpoint's class), checkpoint runs and the WAL roll-blocked wait.
print("\n#### each read core over the window")
print("| arm | run | core | readers at end | nwalker | fg polls >10 ms | fg longest poll | sys polls >10 ms | sys longest poll | checkpoint runs | roll-blocked wait |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for arm, _ in ARMS:
    for r, d in runs(arm):
        ends = d["cores_end"]
        roles = d["roles"]
        for c, m in sorted(d["meta_by_core"].items()):
            ci = int(c)
            nrd = sum(1 for e, ro in zip(ends, roles) if ro == "reader" and int(e) == ci)
            nw = "yes" if any(ro == "nwalker" and int(e) == ci for e, ro in zip(ends, roles)) else ""
            rb = d["meta_delta"].get(f"load_c{ci}_wal_roll_blocked_wait_us", 0)
            print(f"| {arm} | r{r} | {ci} | {nrd} | {nw} | {int(m.get('sched_foreground_polls_over_10ms', 0))} | "
                  f"{fmt(m.get('sched_foreground_longest_poll_us', 0))} | {int(m.get('sched_system_polls_over_10ms', 0))} | "
                  f"{fmt(m.get('sched_system_longest_poll_us', 0))} | {int(m.get('checkpoint_runs', 0))} | {fmt(rb)} |")

print("\n#### batch sessions' work and moves")
print("| arm | run | cores at end (roles in order) | moves (since start) | moves in window | inserter rows/s | walker stmts | errors |")
print("|---|---|---|---|---|---|---|---|")
for arm, _ in ARMS:
    for r, d in runs(arm):
        ends = ", ".join(str(int(c)) for c in d["cores_end"])
        print(f"| {arm} | r{r} | {ends} | {d['moves']} | {d['moves_in_window']} | "
              f"{d.get('inserter_rows_per_s', 0):,.0f} | {(d.get('walker') or {}).get('n', 0):,} | {d['errors']} |")

# ---- forced sharing (same/, the balancer off, every session on core 1) ----
SAME = [("same-iso", "-same-isolated-relaxed-w0-slicedflt-levels-r"),
        ("same-nw", "-same-isolated-relaxed-w0-slicedflt-levels-nw1-r")]


def load_same(arm, r):
    hits = [f for f in glob.glob(f"{HERE}/same/r{r}/bl-mixed-k4-same-*.json") if dict(SAME)[arm] in f]
    return json.load(open(hits[0])) if hits else None


SRUNS = sorted(int(os.path.basename(p)[1:]) for p in glob.glob(f"{HERE}/same/r*") if os.path.isdir(p))
if SRUNS:
    for kind, key in (("schedule", "read_from_schedule"), ("service", "read_service")):
        print(f"\n#### forced sharing: reader {kind} latency, per run and median")
        print("| arm | run | n | " + " | ".join(k[:-3] for k in KEYS) + " |")
        print("|---|---|---|" + "---|" * len(KEYS))
        for arm, _ in SAME:
            ps = []
            for r in SRUNS:
                d = load_same(arm, r)
                if d:
                    p = d[key]
                    ps.append(p)
                    print(f"| {arm} | r{r} | {p['n']} | " + " | ".join(fmt(p[k]) for k in KEYS) + " |")
            if ps:
                print(f"| {arm} | median of {len(ps)} | | " + " | ".join(fmt(statistics.median(p[k] for p in ps)) for k in KEYS) + " |")
    print("\n#### forced sharing: core 1 and the walker")
    print("| arm | run | cores at end | moves | core-1 fg longest poll | core-1 fg polls | nwalker p50 / p99 | nwalker stmts |")
    print("|---|---|---|---|---|---|---|---|")
    for arm, _ in SAME:
        for r in SRUNS:
            d = load_same(arm, r)
            if not d:
                continue
            m = d["meta_by_core"].get("1", {})
            p = d["nwalker"]
            nw = f"{fmt(p['p50_us'])} / {fmt(p['p99_us'])}" if p else "-"
            print(f"| {arm} | r{r} | {', '.join(str(int(c)) for c in d['cores_end'])} | {d['moves']} | "
                  f"{fmt(m.get('sched_foreground_longest_poll_us', 0))} | {int(m.get('sched_foreground_polls', 0)):,} | "
                  f"{nw} | {p.get('n', 0):,} |")
