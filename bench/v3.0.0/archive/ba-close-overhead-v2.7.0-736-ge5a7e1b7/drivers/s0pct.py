"""Scenario 0, the `txn` phase: median over the three passes of each percentile, per cell. usage: s0pct.py"""
import json, statistics as st

B = "/home/cdkbs/bench-runs/ba-close/s0/json/"
for tag in ("s0-c1-g", "s0-c8-g", "s0-c1-s", "s0-c8-s"):
    ps = []
    for p in ("", "r2-", "r3-"):
        d = json.load(open(B + p + tag + ".json"))
        ps.append({x["phase"]: x for x in d["phases"]}["txn"])
    f = lambda k: st.median(x[k] for x in ps)
    print(tag, "ops", [x["ops"] for x in ps], "errors", [x["errors"] for x in ps],
          "p0 %.0f p25 %.0f p50 %.0f p95 %.0f p99 %.0f max %.0f" % (f("p0_us"), f("p25_us"), f("p50_us"), f("p95_us"), f("p99_us"), f("max_us")))
