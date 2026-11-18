#!/usr/bin/env python3
"""The `levels` arm's reader samples at or above 1 ms (the driver's slow
stamps, latency from the schedule), against the server log's roll holds and
checkpoint runs: how many overlap each, and how many slow samples a run
keeps once the overlapping ones are taken out. A run has 4,006 reader
samples, so its p99 is under 1 ms when fewer than about 40 are left.

Arithmetic on samples, not a measurement of a build without the stall: a
sample taken out here may have queued behind another that stays."""
import glob
import json
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
PAT = "-mixed-relaxed-w0-slicedflt-levels-r"


def p99_without(slow, spans, n):
    """Nearest-rank p99 of the run's reader samples with those overlapping
    `spans` taken out; "< 1 ms" when it falls among the unstamped ones."""
    kept = sorted((ns for t0, ns in slow if not any(t0 < e and t0 + ns > s for s, e in spans)), reverse=True)
    left = n - (len(slow) - len(kept))
    above = left - math.ceil(0.99 * left)  # samples strictly above the p99 rank
    return f"{kept[above] / 1e6:.1f} ms" if above < len(kept) else "< 1 ms"


print("| run | reader samples | >= 1 ms | overlap a roll hold | overlap a checkpoint run | left without rolls | left without checkpoint runs | p99 as measured | p99 without rolls | p99 without checkpoint runs |")
print("|---|---|---|---|---|---|---|---|---|---|")
for r in range(1, 6):
    j = [x for x in glob.glob(f"{HERE}/mixed/r{r}/*.json") if PAT in x][0]
    d = json.load(open(j))
    ck, rl = [], []
    for line in open(j[:-5] + ".s.log"):
        m = re.search(r"start_ns=(\d+) end_ns=(\d+)", line)
        if not m:
            continue
        iv = (int(m[1]), int(m[2]))
        if "[checkpoint] run:" in line:
            ck.append(iv)
        elif "[wal] roll" in line:
            rl.append(iv)
    slow = [s for c in d["clients"] if c["role"] == "reader" for s in c["slow"]]
    in_rl = sum(any(t0 < e and t0 + ns > s for s, e in rl) for t0, ns in slow)
    in_ck = sum(any(t0 < e and t0 + ns > s for s, e in ck) for t0, ns in slow)
    n = d["read_from_schedule"]["n"]
    p99 = d["read_from_schedule"]["p99_us"] / 1e3
    print(f"| r{r} | {n} | {len(slow)} | {in_rl} | {in_ck} | {len(slow) - in_rl} | {len(slow) - in_ck} | "
          f"{p99:.1f} ms | {p99_without(slow, rl, n)} | {p99_without(slow, ck, n)} |")
