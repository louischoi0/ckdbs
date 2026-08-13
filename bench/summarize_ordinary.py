#!/usr/bin/env python3
"""The ordinary-statement regression arms, head against base.

These four arms are autocommitted INSERT / UPDATE / SELECT / DELETE, measured
interleaved between the two binaries inside one run, so a machine that drifts
mid-run moves both arms together. `none` and `twin` carry no assertion, so on
those relations `AssertionEnforcer::CommitTxn` returns at its first lookup and
the arms price everything *except* the assertion protocol; `cnt` and `multi`
are where 5384551's reshuffle of the WAL append relative to the page write
actually runs.
"""
import json
import sys
from pathlib import Path

ARMS = ("ac-insert", "ac-update", "ac-select", "ac-delete")
TAGS = ("none", "twin", "cnt", "multi")


def main():
    for p in sorted(Path(sys.argv[1]).glob("*.json")):
        with open(p) as f:
            blob = json.load(f)
        meta = blob["meta"]
        idx = {(r["side"], r["relation"], r["arm"]): r for r in blob["phases"]}
        if not any(k[2] in ARMS for k in idx):
            continue
        print()
        print("%s - %d rows, %d ordinary ops per relation per side"
              % (p.name, meta["rows"], meta["ordinary_ops"]))
        hdr = ("%-10s %-6s %6s | %7s %7s %7s %7s %7s | %7s %7s %7s %7s %7s | %8s"
               % ("arm", "rel", "ops",
                  "H p0", "H p25", "H p50", "H p95", "H p99",
                  "B p0", "B p25", "B p50", "B p95", "B p99", "d p50"))
        print(hdr)
        print("-" * len(hdr))
        for arm in ARMS:
            for tag in TAGS:
                h = idx.get(("head", tag, arm))
                b = idx.get(("base", tag, arm))
                if not h or not b:
                    continue
                print("%-10s %-6s %6d | %7.1f %7.1f %7.1f %7.1f %7.1f | "
                      "%7.1f %7.1f %7.1f %7.1f %7.1f | %8.2f"
                      % (arm, tag, h["ops"],
                         h["p0_us"], h["p25_us"], h["p50_us"], h["p95_us"], h["p99_us"],
                         b["p0_us"], b["p25_us"], b["p50_us"], b["p95_us"], b["p99_us"],
                         h["p50_us"] - b["p50_us"]))
        print()


if __name__ == "__main__":
    main()
