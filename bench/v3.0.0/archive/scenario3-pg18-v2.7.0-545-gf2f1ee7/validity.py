#!/usr/bin/env python3
"""Flag cells whose run window saw a foreign process in logs/monitor.log.

A cell window is [start, end] from `timeline`. Every monitor sample (2 s apart)
inside the window with an `X` line (a process outside this run that used >10 %
of a CPU over its life, or matches ctest/cc1plus/cmake/ninja/make) marks it.
Prints one line per cell: tag, samples, flagged samples, max 1-min loadavg,
and the first offending lines.
"""
import calendar, re, sys, time

RUN = "/home/cdkbs/bench-runs/pg18-f2f1ee7"
cells = {}
for l in open(sys.argv[1] if len(sys.argv) > 1 else f"{RUN}/timeline"):
    m = re.match(r"== (\S+) (start|end) (\S+)", l)
    if m:
        tag, kind, ts = m.groups()
        t = calendar.timegm(time.strptime(ts, "%Y-%m-%dT%H:%M:%SZ"))
        cells.setdefault(tag, {})[kind] = t
samples = []
cur = None
for l in open(f"{RUN}/logs/monitor.log"):
    if l.startswith("T "):
        _, t, a, b, c = l.split()[:5]
        cur = {"t": int(t), "l1": float(a), "x": []}
        samples.append(cur)
    elif cur is not None and l.startswith("  X "):
        if l.split()[4] not in ("postgres", "python3", "initdb", "psql", "pg_ctl"):
            cur["x"].append(l.strip()[2:])
bad = 0
for tag, w in cells.items():
    if "end" not in w:
        continue
    s = [x for x in samples if w["start"] <= x["t"] <= w["end"]]
    fl = [x for x in s if x["x"]]
    mx = max((x["l1"] for x in s), default=0.0)
    if fl:
        bad += 1
    first = fl[0]["x"][0][:110] if fl else ""
    print(f"{tag}\t{len(s)} samples\t{len(fl)} flagged\tmax load1 {mx:.2f}\t{first}")
print(f"# {bad} flagged cells of {len(cells)}", file=sys.stderr)
