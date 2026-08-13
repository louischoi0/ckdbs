#!/usr/bin/env python3
"""Full five-percentile dump of chosen arms from one cell's JSON."""
import json
import sys

path, arms = sys.argv[1], sys.argv[2].split(",")
with open(path) as f:
    blob = json.load(f)
print("K=%d rows=%d" % (blob["meta"]["reservations"], blob["meta"]["rows"]))
hdr = "%-6s %-9s %-6s %6s %8s %7s %7s %7s %7s %7s" % (
    "side", "arm", "rel", "ops", "mean", "p0", "p25", "p50", "p95", "p99")
print(hdr)
print("-" * len(hdr))
for arm in arms:
    for side in ("head", "base"):
        for tag in ("none", "twin", "cnt", "multi"):
            r = next((r for r in blob["phases"]
                      if r["side"] == side and r["relation"] == tag and r["arm"] == arm), None)
            if not r:
                continue
            print("%-6s %-9s %-6s %6d %8.1f %7.1f %7.1f %7.1f %7.1f %7.1f" % (
                side, arm, tag, r["ops"], r["mean_us"], r["p0_us"], r["p25_us"],
                r["p50_us"], r["p95_us"], r["p99_us"]))
