#!/usr/bin/env python3
"""DL-S0 detail: per-lifetime meta and tails. Usage: detail.py raw.jsonl"""
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
print("== meta per lifetime (cells with a captured/cabin/dropped key, plus insert)")
for r in rows:
    if r["cell"] in ("join8", "insert1", "insert8", "cabin8", "evict8"):
        m = {k: v for k, v in r["meta"].items()
             if not k.startswith("contention_checkpoint") and "syncs" not in k}
        print(r["cell"], r["arm"], r["round"], r["stmts"], m)
print("\n== per-lifetime tails")
for r in rows:
    if r["cell"] in ("storm8", "tick8", "select8", "pool8", "join8", "select1"):
        print(r["cell"], r["arm"], r["round"], "p50", r["p50"], "p99", r["p99"], "p999", r["p999"],
              "qps %.0f" % r["qps"], "cpu %.2f" % r["cpu_us_per_stmt"], "load", r["load"], "stmts", r["stmts"])
