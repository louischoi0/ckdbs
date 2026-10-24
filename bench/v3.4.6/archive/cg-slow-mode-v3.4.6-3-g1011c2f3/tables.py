#!/usr/bin/env python3
"""Folds the slow-mode matrix: per arm and rep, the 10,000-row walking UPDATE
(uq-update) - p0/p50/p99 on the wire, the server's on-CPU and run-queue wait
per statement, CPU 4 (the server's) and CPU 5 (its SMT sibling) busy, the
1-minute load, and the most competitors a cell's watcher saw - then the
per-arm medians and how many reps sit in the slow band.

Usage: tables.py OUT [phase] [size]
"""
import glob
import json
import os
import statistics
import sys

out = sys.argv[1]
phase = sys.argv[2] if len(sys.argv) > 2 else "uq-update"
size = sys.argv[3] if len(sys.argv) > 3 else "10000"


def maxcompeting(tag):
    try:
        return max(int(l.split()[1]) for l in open(os.path.join(out, tag + ".watch")) if l.strip())
    except (OSError, ValueError):
        return -1


rows = []
for f in sorted(glob.glob(os.path.join(out, "cg-*-r*.json"))):
    tag = os.path.basename(f)[:-5]
    arm = tag.split("-")[1]
    rep = int(tag.split("-r")[-1])
    c = json.load(open(f))[size][phase]
    rows.append((arm, rep, c, maxcompeting(tag)))

print(f"## {phase} at {size} rows, per rep\n")
print("| arm | rep | p0 | p50 | p99 | server CPU us/stmt | run-queue wait us/stmt | CPU4 busy | CPU5 busy | load1 | competitors seen |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for arm, rep, c, comp in sorted(rows, key=lambda r: (r[0], r[1])):
    b = c.get("cpu_busy_pct", {})
    print(f"| {arm} | {rep} | {c['p0']} | {c['p50']} | {c['p99']} | {c.get('server_cpu_us')} | "
          f"{c.get('server_wait_us')} | {b.get('4')} | {b.get('5')} | {c.get('load1')} | {comp} |")

print(f"\n## Medians per arm\n")
print("| arm | reps | p50 | server CPU us/stmt | wait us/stmt | CPU5 busy | p50 per rep (sorted) |")
print("|---|---|---|---|---|---|---|")
for arm in ("A", "B", "Ba"):
    cs = [r[2] for r in rows if r[0] == arm]
    if not cs:
        continue
    med = lambda k: round(statistics.median(c[k] for c in cs if c.get(k) is not None), 1)  # noqa: E731
    sib = round(statistics.median(c["cpu_busy_pct"]["5"] for c in cs if "cpu_busy_pct" in c), 1)
    print(f"| {arm} | {len(cs)} | {med('p50')} | {med('server_cpu_us')} | {med('server_wait_us')} | {sib} | "
          + " / ".join(str(c["p50"]) for c in sorted(cs, key=lambda c: c["p50"])) + " |")
