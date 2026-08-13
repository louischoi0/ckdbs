#!/usr/bin/env python3
"""Collapses the abort sweep's per-cell JSON into the two tables that answer it.

Table 1 - the assertion protocol's own cost on each ending, per side. Every
number is a *difference against the unasserted control on the same side in the
same run*, which is what cancels the socket round trip, BEGIN, the K INSERTs
and the undo unwind, leaving only what the assertion protocol added. `twin`
is the same subtraction over a second unasserted relation, so it is the
in-run noise floor and any row below it is not a finding.

Table 2 - the double difference head-minus-base on the rollback arm, which is
exactly what 67ce947 added to abort, against the same double difference on the
commit arm, which 67ce947 was not supposed to change.
"""
import json
import sys
from pathlib import Path


def load(d):
    cells = {}
    for p in sorted(Path(d).glob("*.json")):
        with open(p) as f:
            blob = json.load(f)
        meta = blob["meta"]
        idx = {(r["side"], r["relation"], r["arm"]): r for r in blob["phases"]}
        cells[(meta["rows"], meta["reservations"])] = (meta, idx)
    return cells


def p50(idx, side, tag, arm):
    r = idx.get((side, tag, arm))
    return r["p50_us"] if r else float("nan")


def p25(idx, side, tag, arm):
    r = idx.get((side, tag, arm))
    return r["p25_us"] if r else float("nan")


def main():
    cells = load(sys.argv[1])
    keys = sorted(cells)

    print()
    print("Table 1 - assertion cost per transaction (p50 us), as a delta against")
    print("the unasserted control in the same run. `twin` is the noise floor.")
    hdr = ("%5s %4s | %7s %7s %7s | %7s %7s %7s | %7s %7s %7s | %7s %7s %7s"
           % ("rows", "K",
              "H rb tw", "H rb cnt", "H rb mlt",
              "H ci tw", "H ci cnt", "H ci mlt",
              "B rb tw", "B rb cnt", "B rb mlt",
              "B ci tw", "B ci cnt", "B ci mlt"))
    print(hdr)
    print("-" * len(hdr))
    for k in keys:
        meta, idx = cells[k]
        vals = []
        for side in ("head", "base"):
            for arm in ("rollback", "commit"):
                base_v = p50(idx, side, "none", arm)
                for tag in ("twin", "cnt", "multi"):
                    vals.append(p50(idx, side, tag, arm) - base_v)
        # reorder to H rb, H ci, B rb, B ci
        print("%5d %4d | %7.2f %8.2f %8.2f | %7.2f %8.2f %8.2f | %7.2f %8.2f %8.2f | %7.2f %8.2f %8.2f"
              % (k[0], k[1], *vals))

    print()
    print("Table 2 - the double difference: (asserted - control) on head minus the")
    print("same on base. The rollback column is what 67ce947 added to abort; the")
    print("commit column is the same statistic on a path it did not change, and is")
    print("therefore the control on the whole method.")
    hdr2 = ("%5s %4s | %12s %12s | %12s %12s | %10s"
            % ("rows", "K", "rb cnt dd", "rb multi dd", "ci cnt dd", "ci multi dd",
               "floor dd"))
    print(hdr2)
    print("-" * len(hdr2))
    for k in keys:
        meta, idx = cells[k]
        out = []
        for arm in ("rollback", "commit"):
            for tag in ("cnt", "multi"):
                h = p50(idx, "head", tag, arm) - p50(idx, "head", "none", arm)
                b = p50(idx, "base", tag, arm) - p50(idx, "base", "none", arm)
                out.append(h - b)
        fh = p50(idx, "head", "twin", "rollback") - p50(idx, "head", "none", "rollback")
        fb = p50(idx, "base", "twin", "rollback") - p50(idx, "base", "none", "rollback")
        out.append(fh - fb)
        print("%5d %4d | %12.2f %12.2f | %12.2f %12.2f | %10.2f" % (k[0], k[1], *out))

    print()
    print("Table 3 - per-reservation: the cnt-relation assertion cost divided by K.")
    print("A per-reservation cost is flat down this column; a per-transaction one")
    print("falls as 1/K.")
    hdr3 = "%5s %4s | %14s %14s | %14s %14s" % (
        "rows", "K", "H rollback/K", "H commit/K", "B rollback/K", "B commit/K")
    print(hdr3)
    print("-" * len(hdr3))
    for k in keys:
        meta, idx = cells[k]
        K = k[1]
        row = []
        for side in ("head", "base"):
            for arm in ("rollback", "commit"):
                row.append((p50(idx, side, "cnt", arm) - p50(idx, side, "none", arm)) / K)
        print("%5d %4d | %14.3f %14.3f | %14.3f %14.3f" % (k[0], K, *row))

    print()
    print("Table 4 - absolute p50/p25 of the rollback and commit arms, cnt relation.")
    hdr4 = "%5s %4s | %9s %9s %9s %9s | %9s %9s" % (
        "rows", "K", "H rb p25", "H rb p50", "H ci p25", "H ci p50", "B rb p50", "B ci p50")
    print(hdr4)
    print("-" * len(hdr4))
    for k in keys:
        meta, idx = cells[k]
        print("%5d %4d | %9.2f %9.2f %9.2f %9.2f | %9.2f %9.2f" % (
            k[0], k[1],
            p25(idx, "head", "cnt", "rollback"), p50(idx, "head", "cnt", "rollback"),
            p25(idx, "head", "cnt", "commit"), p50(idx, "head", "cnt", "commit"),
            p50(idx, "base", "cnt", "rollback"), p50(idx, "base", "cnt", "commit")))
    print()


if __name__ == "__main__":
    main()
