#!/usr/bin/env python3
"""Per-statement wait counts and us for every contention latch, mean over lifetimes of an arm. Usage: topwaits.py raw.jsonl"""
import json, sys, statistics as st
from collections import defaultdict
rows = [json.loads(l) for l in open(sys.argv[1])]
g = defaultdict(list)
for r in rows:
    g[(r["cell"], r["arm"])].append(r)
for (cell, arm), rs in g.items():
    if cell not in ("select1", "select8", "insert1", "insert8", "cabin8", "evict8"):
        continue
    keys = sorted({k[len("contention_"):-len("_waits")] for r in rs for k in r["meta"]
                   if k.endswith("_waits") and k.startswith("contention_")})
    out = []
    for k in keys:
        c = st.mean(r["meta"].get(f"contention_{k}_waits", 0) / r["stmts"] for r in rs)
        u = st.mean(r["meta"].get(f"contention_{k}_wait_us", 0) / r["stmts"] for r in rs)
        out.append(f"{k} {c:.4f}/{u:.3f}us")
    print(f"{cell} {arm}: " + "; ".join(out))
