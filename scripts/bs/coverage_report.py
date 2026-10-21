#!/usr/bin/env python3
"""BS-S2: llvm-cov's summary folded per src/ subsystem, against the floor.

    coverage_report.py COVERAGE_JSON FLOOR_FILE REPO_ROOT [--record]

Prints one line per subsystem - lines and branches covered - and its floor;
exits 1 if any subsystem is below its floor (BS-Q4 (b)). `--record` writes the
floor file with each value raised to what ran, never lowered.
"""
import collections
import json
import os
import sys


TOLERANCE = 0.2


def main():
    cov_path, floor_path, root = sys.argv[1:4]
    record = "--record" in sys.argv[4:]
    src = os.path.realpath(os.path.join(root, "src")) + os.sep
    totals = collections.defaultdict(lambda: [0, 0, 0, 0])  # lines hit/all, branches hit/all
    with open(cov_path) as f:
        data = json.load(f)
    for entry in data["data"][0]["files"]:
        path = os.path.realpath(entry["filename"])
        if not path.startswith(src):
            continue
        rel = path[len(src):]
        subsystem = "src/" + rel.split(os.sep)[0] if os.sep in rel else "src"
        s = entry["summary"]
        t = totals[subsystem]
        t[0] += s["lines"]["covered"]
        t[1] += s["lines"]["count"]
        t[2] += s["branches"]["covered"]
        t[3] += s["branches"]["count"]

    floor = {}
    if os.path.exists(floor_path):
        with open(floor_path) as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    d, lp, bp = line.split()
                    floor[d] = (float(lp), float(bp))

    below = []
    rows = []
    for d in sorted(totals):
        lh, la, bh, ba = totals[d]
        lp = 100.0 * lh / la if la else 100.0
        bp = 100.0 * bh / ba if ba else 100.0
        fl = floor.get(d)
        mark = ""
        # Floors are recorded to one decimal, so compare at that precision.
        # A tolerance of TOLERANCE points: a branch taken by timing alone
        # moves a small subsystem's figure by more than the 0.1 recorded.
        if fl is not None and (lp < fl[0] - TOLERANCE or bp < fl[1] - TOLERANCE):
            below.append(d)
            mark = "  BELOW FLOOR"
        shown = f"{fl[0]:.1f} / {fl[1]:.1f}" if fl else "none"
        rows.append((d, lp, bp))
        print(f"{d:24s} lines {lp:5.1f} % ({lh}/{la})  branches {bp:5.1f} % ({bh}/{ba})  "
              f"floor {shown}{mark}")
    # A floored subsystem the run did not report at all is below its floor,
    # not exempt from it; --record keeps its line.
    for d in sorted(set(floor) - set(totals)):
        below.append(d)
        rows.append((d, 0.0, 0.0))
        print(f"{d:24s} not in the run  floor {floor[d][0]:.1f} / {floor[d][1]:.1f}  BELOW FLOOR")

    if record:
        with open(floor_path, "w") as f:
            f.write("# BS-S2's coverage floor (scripts/bs/coverage.sh): <src dir> <line %> "
                    "<branch %>.\n# A ratchet: --record raises a value and never lowers one.\n")
            for d, lp, bp in sorted(rows):
                old = floor.get(d, (0.0, 0.0))
                f.write(f"{d} {max(old[0], round(lp, 1)):.1f} {max(old[1], round(bp, 1)):.1f}\n")
        print(f"coverage: floor written to {floor_path}")
    if below:
        print("coverage: below the floor in " + ", ".join(below))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
