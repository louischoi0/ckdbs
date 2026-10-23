#!/usr/bin/env python3
"""CG close tables, from cg-<A|B>-r<rep>.json in the directory named (default:
beside this file; `python3 tables.py rerun` for the second rotation)
(tools/cg_close_benchmark.py output). A table entry is the median over the
reps of that percentile, in microseconds."""
import glob, json, os, re, statistics, sys

HERE = (os.path.abspath(sys.argv[1]) if len(sys.argv) > 1
        else os.path.dirname(os.path.abspath(__file__)))
KEYS = ["p0", "p25", "p50", "p90", "p99", "max"]
runs = {"A": {}, "B": {}}
for f in glob.glob(f"{HERE}/cg-*-r*.json"):
    arm, rep = re.search(r"cg-([AB])-r(\d+)\.json$", f).groups()
    runs[arm][int(rep)] = json.load(open(f))


def vals(arm, size, phase, key):
    return [r[size][phase][key] for r in runs[arm].values() if phase in r[size]]


print("#### Medians over reps (us)")
print("| rows | phase | arm | reps | " + " | ".join(KEYS) + " | mean | p50 per rep |")
print("|---|---|---|---|" + "---|" * len(KEYS) + "---|---|")
for size in ("1000", "10000"):
    for phase in ("insert", "dn-insert", "dn-skip", "update", "uq-update",
                  "up-insert", "up-update", "uq-upsert"):
        for arm in ("A", "B"):
            p50 = vals(arm, size, phase, "p50")
            if not p50:
                continue
            cells = [statistics.median(vals(arm, size, phase, k)) for k in KEYS]
            mean = statistics.median(vals(arm, size, phase, "mean"))
            print(f"| {size} | {phase} | {arm} | {len(p50)} | " +
                  " | ".join(f"{c:,.1f}" for c in cells) + f" | {mean:,.1f} | " +
                  " / ".join(f"{v:,.1f}" for v in sorted(p50)) + " |")

print("\n#### A -> B, p50 (median of rep p50s), and whether the rep ranges overlap")
print("| rows | phase | A | B | delta | overlap? | B above A in paired reps |")
print("|---|---|---|---|---|---|---|")
for size in ("1000", "10000"):
    for phase in ("insert", "dn-insert", "dn-skip", "update", "uq-update"):
        a, b = vals("A", size, phase, "p50"), vals("B", size, phase, "p50")
        if not a or not b:
            continue
        ma, mb = statistics.median(a), statistics.median(b)
        ov = "yes" if not (max(b) < min(a) or max(a) < min(b)) else "no"
        reps = sorted(set(runs["A"]) & set(runs["B"]))
        above = sum(1 for r in reps if runs["B"][r][size][phase]["p50"] > runs["A"][r][size][phase]["p50"])
        print(f"| {size} | {phase} | {ma:,.1f} | {mb:,.1f} | {100 * (mb / ma - 1):+.1f} % | {ov} | {above}/{len(reps)} |")

print("\n#### B only: each DO UPDATE arm against its plain counterpart, p50")
print("| rows | arm | p50 | counterpart | p50 | delta |")
print("|---|---|---|---|---|---|")
for size in ("1000", "10000"):
    for phase, base in (("up-insert", "insert"), ("up-update", "update"), ("uq-upsert", "uq-update")):
        x, y = vals("B", size, phase, "p50"), vals("B", size, base, "p50")
        if not x or not y:
            continue
        mx, my = statistics.median(x), statistics.median(y)
        print(f"| {size} | {phase} | {mx:,.1f} | {base} | {my:,.1f} | {100 * (mx / my - 1):+.1f} % |")
