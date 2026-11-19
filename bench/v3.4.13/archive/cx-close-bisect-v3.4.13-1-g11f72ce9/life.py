#!/usr/bin/env python3
"""Per lifetime: the round-paired median server-CPU delta (us) of each pair, a markdown table.
A round's cell with a competing build or test process in any block is dropped.
usage: life.py E-B,E-B,... out1.json out2a.json ..."""
import collections, json, statistics, sys
pairs = [p.split("-") for p in sys.argv[1].split(",")]
files = sys.argv[2:]
cells = ["pk-point-read", "point-join-2rel", "point-join-3rel"]
print("| Pair | Cell | " + " | ".join(f.replace("out", "").replace(".json", "") for f in files) + " |")
print("|---|---|" + "---|" * len(files))
D = []
for f in files:
    blocks = json.load(open(f))
    bad = {(r["round"], r["cell"]) for r in blocks if r["host"]["competing"]}
    d = collections.defaultdict(dict)
    for r in blocks:
        if (r["round"], r["cell"]) not in bad: d[(r["cell"], r["engine"])][r["round"]] = r["server_cpu_us"]
    D.append(d)
    load = [float(r["host"]["loadavg"].split()[0]) for r in blocks]
    print(f"<!-- {f}: {len(blocks)} blocks, load {min(load)}..{max(load)}, competing blocks {sum(1 for r in blocks if r['host']['competing'])} -->", file=sys.stderr)
for e, b in pairs:
    for c in cells:
        row = []
        for d in D:
            if (c, e) not in d or (c, b) not in d: row.append("-"); continue
            rs = sorted(set(d[(c, e)]) & set(d[(c, b)]))
            row.append(f"{statistics.median(d[(c, e)][r] - d[(c, b)][r] for r in rs):+.2f} (n={len(rs)})")
        print(f"| {e} - {b} | {c} | " + " | ".join(row) + " |")
