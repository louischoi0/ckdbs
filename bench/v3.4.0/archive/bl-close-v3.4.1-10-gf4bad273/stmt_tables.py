#!/usr/bin/env python3
"""BL-S5's statement-path tables, from stmt/stmt-<arm>-k<cores>-r<rep>.json beside
this file (tools/a1_close_benchmark.py --mode stmt output). A table entry is the
median over the reps of that percentile, microseconds; the delta table sets each
arm's rep p50s against the other's."""
import glob, json, os, re, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
import sys
SUB = sys.argv[1] if len(sys.argv) > 1 else "stmt"
KEYS = ["p0", "p25", "p50", "p90", "p99", "max"]
STMTS = ["insert", "point-select", "update", "full-scan"]
runs = {}
for f in glob.glob(f"{HERE}/" + SUB + "/stmt-*-k*-r*.json"):
    arm, cores, rep = re.search(r"stmt-(\w)-k(\d)-r(\d+)\.json$", f).groups()
    runs.setdefault((arm, int(cores)), {})[int(rep)] = json.load(open(f))


def med(arm, cores, size, stmt, key):
    vals = [r[size][stmt][key] for r in runs[(arm, cores)].values()]
    return statistics.median(vals), vals


print("#### Medians over reps (µs)")
print("| cores | rows | statement | arm | reps | " + " | ".join(KEYS) + " | p50 per rep |")
print("|---|---|---|---|---|" + "---|" * len(KEYS) + "---|")
for cores in (1, 4):
    for size in ("200", "1000", "10000"):
        for stmt in STMTS:
            for arm in ("A", "B", "N"):
                if (arm, cores) not in runs:
                    continue
                cells = [med(arm, cores, size, stmt, k)[0] for k in KEYS]
                p50s = sorted(med(arm, cores, size, stmt, "p50")[1])
                print(f"| {cores} | {size} | {stmt} | {arm} | {len(runs[(arm, cores)])} | "
                      + " | ".join(f"{c:,.1f}" for c in cells)
                      + " | " + " / ".join(f"{v:,.1f}" for v in p50s) + " |")

print("\n#### p50 deltas (median of rep p50s), and whether the rep ranges overlap")
print("| cores | rows | statement | pair | first | second | delta | ranges overlap? |")
print("|---|---|---|---|---|---|---|---|")
for cores, pairs in ((1, [("A", "B")]), (4, [("A", "B"), ("N", "B")])):
    for size in ("200", "1000", "10000"):
        for stmt in STMTS:
            for x, y in pairs:
                if (x, cores) not in runs or (y, cores) not in runs:
                    continue
                mx, vx = med(x, cores, size, stmt, "p50")
                my, vy = med(y, cores, size, stmt, "p50")
                ov = "yes" if not (max(vy) < min(vx) or max(vx) < min(vy)) else "no"
                print(f"| {cores} | {size} | {stmt} | {x}->{y} | {mx:,.1f} | {my:,.1f} | "
                      f"{100 * (my / mx - 1):+.1f} % | {ov} |")
