#!/usr/bin/env python3
"""Concatenate lifetimes' block lists, round ids offset so a round pairs within its lifetime.
A round's cell (its blocks on every arm) is dropped where any of those blocks saw a competing build
or test process (the host record).
usage: merge.py out.json in1.json in2.json ..."""
import json, sys
out = []
for i, f in enumerate(sys.argv[2:]):
    blocks = json.load(open(f))
    bad = {(r["round"], r["cell"]) for r in blocks if r["host"]["competing"]}
    kept = [r for r in blocks if (r["round"], r["cell"]) not in bad]
    print(f"{f}: {len(blocks)} blocks, {len(blocks) - len(kept)} dropped (competing process in the round's cell)", file=sys.stderr)
    for r in kept:
        r["lifetime"] = i + 1; r["round"] += 1000 * i; out.append(r)
json.dump(out, open(sys.argv[1], "w"))
