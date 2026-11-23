#!/usr/bin/env python3
"""DQ-S0's table: per (n, dist, arm, statement), the median over rounds of a
`dq_s0_premise.py` results file - server CPU, the inner step's walks, and the
Cabin and build counters - and the statement at which A first meets B.

    dq_s0_report.py RESULTS.json [RESULTS.json ...]
"""
import json, sys
from statistics import median as med



def main():
    by = {}
    for path in sys.argv[1:]:
        for c in json.load(open(path))["cells"]:
            for s, st in enumerate(c["stmts"]):
                by.setdefault((c["n"], c["dist"], c["arm"], s + 1), []).append(st)
    print("| inner rows | outer keys | arm | statement | server CPU, median | min | max | misses | "
          "hits | recorded | built | refused |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for key in sorted(by, key=lambda k: (k[0], k[1], ["A", "Agate", "B"].index(k[2]), k[3])):
        v = by[key]
        ok = [x for x in v if "refused" not in x]
        cpu = [x["cpu_us"] / 1000 for x in v]

        def m(f, unit):
            return f"{med([x[f] for x in ok]):g} {unit}" if ok else "-"
        print(f"| {key[0]:,} rows | {key[1]} | {key[2]} | #{key[3]} | {med(cpu):.2f} ms | "
              f"{min(cpu):.2f} ms | {max(cpu):.2f} ms | {m('cabin_misses', 'misses')} | "
              f"{m('cabin_hits', 'hits')} | {m('cabin_recorded', 'sets')} | "
              f"{m('inner_built', 'maps')} | {len(v) - len(ok)} of {len(v)} |")
    # Where A (and Agate) first costs no more than B, statement by statement.
    print()
    print("| inner rows | outer keys | arm | first statement at or below B's CPU | "
          "A/B CPU at statement 1 |")
    print("|---|---|---|---|---|")
    keys = sorted({(k[0], k[1]) for k in by})
    for n, dist in keys:
        stmts = sorted({k[3] for k in by if k[0] == n and k[1] == dist})
        b = {s: med([x["cpu_us"] for x in by[(n, dist, "B", s)]]) for s in stmts}
        for arm in ("A", "Agate"):
            if (n, dist, arm, 1) not in by:
                continue
            a = {s: med([x["cpu_us"] for x in by[(n, dist, arm, s)]]) for s in stmts}
            meet = next((s for s in stmts if a[s] <= b[s]), None)
            meet_text = f"#{meet}" if meet else f"none of {len(stmts)} statements"
            # A B reading of 0 us (a sub-tick read, DQ-S0's resolution note)
            # has no ratio; dividing by it would end the report.
            ratio = f"{a[1] / b[1]:.0f}x" if b[1] > 0 else "- (B read 0 us)"
            print(f"| {n:,} rows | {dist} | {arm} | {meet_text} | {ratio} |")


if __name__ == "__main__":
    main()
