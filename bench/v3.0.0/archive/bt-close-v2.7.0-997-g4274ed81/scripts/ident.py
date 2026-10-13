#!/usr/bin/env python3
"""BT-R3's identity check: ANALYZE counts and the row digest per arm, run 0 of each file.

    ident.py LABEL=FILE [LABEL=FILE ...]
"""
import gzip, json, re, sys

print("| file | cell | arm | examined | matched | sel | pages | spills | row digest |")
print("|---|---|---|---|---|---|---|---|---|")
bad = 0
for a in sys.argv[1:]:
    lab, p = a.split("=", 1)
    d = json.load(gzip.open(p, "rt") if p.endswith(".gz") else open(p))
    r0 = d["runs"][0]
    for cell, per in r0["analyze"].items():
        seen = set()
        for arm in ("A", "A2", "B"):
            v = per[arm]
            m = re.search(r"step 0 \w+ \w+ opens=\d+ examined=(\d+) matched=(\d+) sel=(\d+)% pages=(\d+)(?: spills=(\d+))?", v)
            dig = r0["digest"][cell][arm]
            seen.add((m.groups(), dig))
            print("| %s | %s | %s | %s rows | %s row | %s %% | %s pages | %s | `%s` |" % (
                lab, cell, {"A": "A", "A2": "A'", "B": "B"}[arm], format(int(m.group(1)), ","), m.group(2), m.group(3),
                m.group(4), m.group(5) or "0", dig))
        if len(seen) != 1:
            bad += 1
print("\ncells whose arms differ in counts or digest: %d" % bad)
