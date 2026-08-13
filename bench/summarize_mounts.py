#!/usr/bin/env python3
"""Collapses the mount matrix into one table per row-set size.

Each cell is nine mounts of one data file. `noassert` is the control: the same
relation and the same load with no assertion declared, so RC07's revival and
`AttachEntriesFromPages` have nothing to walk. `assert` declares the ceiling
before the load, so every loaded row leaves one Bound Cabin entry for the walk.
`assertrb` rolls back every 4th row in its own transaction, so a quarter of the
entries on the page are orphaned - which is the shape 67ce947 changed, because
the head binary's walk skips a marked entry and the base binary's cannot.
"""
import json
import sys
from pathlib import Path

SHAPES = ("noassert", "assert", "assertrb")


def main():
    cells = {}
    for p in sorted(Path(sys.argv[1]).glob("*.json")):
        with open(p) as f:
            blob = json.load(f)
        label = blob["meta"]["label"]
        side, rows, shape = label.split("-")
        cells[(int(rows[1:]), side, shape)] = blob

    rowsets = sorted({k[0] for k in cells})
    for rows in rowsets:
        print()
        print("rows = %d  (entries the walk attributes: %d under `assert`)" % (rows, rows))
        hdr = ("%-6s %-9s | %7s %7s %7s %7s %7s | %8s %7s %7s %7s"
               % ("side", "shape", "p0", "p25", "p50", "p95", "p99",
                  "analysis", "redo", "ckpt", "resid"))
        print(hdr)
        print("-" * len(hdr))
        for side in ("head", "base"):
            for shape in SHAPES:
                blob = cells.get((rows, side, shape))
                if not blob:
                    continue
                s = blob["phase"]
                ms = [m for m in blob["mounts"]]
                def med(key):
                    vals = sorted(m[key] for m in ms if key in m)
                    return vals[len(vals) // 2] / 1000.0 if vals else float("nan")
                an, rd, ck = med("recovery_analysis_us"), med("recovery_redo_us"), \
                    med("recovery_checkpoint_us")
                hw, un = med("recovery_high_water_us"), med("recovery_undo_us")
                resid = s["p50_us"] / 1000.0 - (an + rd + ck + hw + un)
                print("%-6s %-9s | %7.1f %7.1f %7.1f %7.1f %7.1f | %8.1f %7.1f %7.1f %7.1f"
                      % (side, shape, s["p0_us"] / 1000, s["p25_us"] / 1000,
                         s["p50_us"] / 1000, s["p95_us"] / 1000, s["p99_us"] / 1000,
                         an, rd, ck, resid))
        # What the assertion walk costs: assert minus noassert, same side.
        print()
        for side in ("head", "base"):
            base_c = cells.get((rows, side, "noassert"))
            if not base_c:
                continue
            b50 = base_c["phase"]["p50_us"] / 1000
            for shape in ("assert", "assertrb"):
                c = cells.get((rows, side, shape))
                if not c:
                    continue
                print("  %-4s %-9s p50 %7.1f ms   vs its own noassert control: %+6.1f ms"
                      % (side, shape, c["phase"]["p50_us"] / 1000,
                         c["phase"]["p50_us"] / 1000 - b50))
    print()


if __name__ == "__main__":
    main()
