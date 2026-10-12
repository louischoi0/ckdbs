#!/usr/bin/env python3
"""Summarize BL-S1 mixed cells. Usage: analyze_mixed.py DIR [DIR...]
Per (cores, placement, arm, slice): runs, and the median across runs of the
point reader's service p50/p99 and from-schedule p99/max, with the min and
max of the from-schedule p99 so run-to-run spread is visible."""
import collections, glob, json, statistics, sys

rows = collections.defaultdict(list)
for d in sys.argv[1:]:
    for f in glob.glob(d + "/**/*.json", recursive=True):
        r = json.load(open(f))
        key = (r["cores"], r.get("placement", "same"), r["arm"], r.get("slice") or 64,
               "ws-off" if "ws-off" in f or "attr2" in f else "ws-on")
        rows[key].append(r)


def med(rs, part, q):
    xs = [r[part].get(q) for r in rs if r[part].get(q) is not None]
    return statistics.median(xs) if xs else float("nan")


print(f"{'cores':>5} {'placement':14} {'arm':8} {'slice':>5} {'ws':6} {'n':>2} "
      f"{'svc p50':>8} {'svc p99':>9} {'sched p99':>10} {'(min..max)':>22} {'max':>9}")
for key in sorted(rows):
    rs = rows[key]
    p99s = [r["read_from_schedule"].get("p99_us", float("nan")) for r in rs]
    print(f"{key[0]:5} {key[1]:14} {key[2]:8} {key[3]:5} {key[4]:6} {len(rs):2} "
          f"{med(rs, 'read_service', 'p50_us'):8.0f} {med(rs, 'read_service', 'p99_us'):9.0f} "
          f"{statistics.median(p99s):10.0f} ({min(p99s):9.0f}..{max(p99s):9.0f}) "
          f"{med(rs, 'read_from_schedule', 'max_us'):9.0f}")
