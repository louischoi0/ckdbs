"""Ranges of a fragment's ratio and spread columns, for the prose. Usage: python3 -I ranges.py"""
import glob, os, re, statistics

T = "/home/ukjin/bench-runs/scenarios-v3.4.17-159-g9f8f8b354/out/tables"
for f in sorted(glob.glob(T + "/*.md")):
    name = os.path.basename(f)[:-3]
    rows = [l for l in open(f) if l.startswith("|") and not l.startswith("|---")]
    if len(rows) < 2:
        continue
    head = [c.strip() for c in rows[0].strip().strip("|").split("|")]
    out = []
    for ci, h in enumerate(head):
        vals = []
        for r in rows[1:]:
            c = [x.strip() for x in r.strip().strip("|").split("|")]
            if ci < len(c):
                m = re.fullmatch(r"(\d+\.\d+)x", c[ci]) or re.fullmatch(r"(\d+\.\d+) %", c[ci])
                if m:
                    vals.append(float(m.group(1)))
        if len(vals) >= 2 and ("over" in h or "spread" in h):
            out.append(f"[{h[:38]}] min {min(vals)} med {statistics.median(vals):.2f} max {max(vals)} n {len(vals)}")
    reads = {}
    if head[-1] == "read":
        for r in rows[1:]:
            c = [x.strip() for x in r.strip().strip("|").split("|")]
            reads[c[-1]] = reads.get(c[-1], 0) + 1
    if out or reads:
        print(name, "::", " ; ".join(out), reads or "")
