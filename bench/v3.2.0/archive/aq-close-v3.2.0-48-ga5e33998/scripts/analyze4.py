#!/usr/bin/env python3
"""Section 8 tables (the UPDATE fix re-measured): four arms A, A2 (A'), B, Bp (B').
Usage: analyze4.py FILE... (phase JSONs under out/, runs pooled)."""
import json, statistics as st, sys
med = st.median
R = "/home/cdkbs/bench-runs/aq-close/out/"
runs = []
for f in sys.argv[1:]:
    runs += json.load(open(R + f + ".json"))["runs"]
print("runs", len(runs))
for sh in ("update-range", "select-pk"):
    for path in ("literal", "prepared"):
        rows = []
        for r in runs:
            d = {a["bin"]: a for a in r["arms"] if a["shape"] == sh and a["path"] == path}
            if len(d) == 4:
                rows.append(d)
        v = lambda arm: [x[arm]["server_cpu_us"] for x in rows]
        n = len(rows)

        def pair(x, y):
            d = [p - q for p, q in zip(v(x), v(y))]
            return d
        print(f"\n## {sh} {path}  n={n} pairs; server CPU us median: " +
              " ".join(f"{k}={med(v(k)):.2f}" for k in ("A", "A2", "B", "Bp")))
        aa = pair("A2", "A")
        print(f"  A'-A: med {med(aa):+.2f} [{min(aa):+.2f},{max(aa):+.2f}] largest|.|={max(abs(x) for x in aa):.2f} typical|.|={med([abs(x) for x in aa]):.2f}")
        for x, y, lab in (("B", "A", "B-A"), ("Bp", "A", "B'-A"), ("Bp", "B", "B'-B"), ("Bp", "A2", "B'-A'"), ("B", "A2", "B-A'")):
            d = pair(x, y)
            print(f"  {lab}: med {med(d):+.2f} [{min(d):+.2f},{max(d):+.2f}] ({100*med(d)/med(v(y)):+.1f} % of {y}); {x} above {y} in {sum(1 for e in d if e > 0)} of {n}")
        print("  per-run: " + " | ".join(" ".join(f"{k} {x[k]['server_cpu_us']:.1f}" for k in ("A", "A2", "B", "Bp")) for x in rows))
        print("  | arm | ops | p0 | p25 | p50 | p90 | p99 | QPS at mean | server CPU | driver CPU |")
        for arm in ("A", "A2", "B", "Bp"):
            L = [x[arm] for x in rows]
            m = lambda k: med([e[k] for e in L])
            print(f"  | {arm} | {sum(e['ops'] for e in L)} | {m('p0_us'):.1f} | {m('p25_us'):.1f} | {m('p50_us'):.1f} | {m('p90_us'):.1f} | {m('p99_us'):.1f} | {1e6/m('mean_us'):.0f} | {m('server_cpu_us'):.1f} | {m('driver_cpu_us'):.1f} |")
bad = sum(a["bad"] for r in runs for a in r["arms"])
unclean = [(i, a["shape"], a["path"], a["bin"]) for i, r in enumerate(runs) for a in r["arms"] if a.get("host", {}).get("clean") is False]
print("\nbad replies", bad, "unclean", len(unclean), unclean[:10])
print("gate loads", [r["quiet_gate"]["load"] for r in runs])
for i, r in enumerate(runs):
    print("run", i, "end_state", {k: {kk: vv for kk, vv in v.items() if kk != "cabins"} for k, v in r.get("end_state", {}).items()})
