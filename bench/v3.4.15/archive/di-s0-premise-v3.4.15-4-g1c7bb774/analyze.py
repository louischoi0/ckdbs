#!/usr/bin/env python3
"""DI-S0 analysis: python3 analyze.py raw.jsonl"""
import json, sys, statistics as st
rows = [json.loads(l) for l in open(sys.argv[1])]
arms = ["A", "A0", "B1", "B2"]
cells = sorted({r["cn"] for r in rows})
by = {(r["cn"], r["arm"], r["round"]): r for r in rows}
rounds = sorted({r["round"] for r in rows})


def get(cn, arm, f):
    return [by[(cn, arm, k)][f] for k in rounds if (cn, arm, k) in by]


print("== per cell and arm (CPU us/stmt, QPS; spread = max - min over lifetimes)")
for cn in cells:
    for a in arms:
        c = get(cn, a, "cpu_us_per_stmt"); q = get(cn, a, "qps"); n = get(cn, a, "stmts")
        print(f"{cn} {a} n_life={len(c)} cpu_mean={st.mean(c):.2f} cpu_spread={max(c)-min(c):.2f} "
              f"qps_mean={st.mean(q):.0f} qps_spread={max(q)-min(q):.0f} stmts={sum(n)} bad={sum(get(cn,a,'bad'))}")
print("== latency (us), median of per-lifetime percentiles")
for cn in cells:
    for a in arms:
        print(cn, a, sum(get(cn, a, "stmts")), *[f"{p}={st.median(get(cn,a,p)):g}" for p in ("p0", "p25", "p50", "p90", "p99")])
print("== paired deltas per round (X - Y), CPU us/stmt and QPS %")
pairs = [("A0", "A"), ("B1", "A"), ("B2", "A"), ("B1", "B2")]
for cn in cells:
    A = get(cn, "A", "cpu_us_per_stmt")
    ca = st.mean(A)
    for x, y in pairs:
        X = get(cn, x, "cpu_us_per_stmt"); Y = get(cn, y, "cpu_us_per_stmt")
        d = [p - q for p, q in zip(X, Y)]
        qx = get(cn, x, "qps"); qy = get(cn, y, "qps")
        dq = [100 * (p - q) / q for p, q in zip(qx, qy)]
        sp = max(max(X) - min(X), max(Y) - min(Y))
        slower = sum(1 for v in d if v > 0)
        print(f"{cn} {x}-{y}: " + " ".join(f"{v:+.2f}" for v in d) +
              f" | mean={st.mean(d):+.2f} us ({100*st.mean(d)/ca:+.1f}% of A) slower_in={slower}/{len(d)}"
              f" sd_of_delta={st.pstdev(d):.2f} arm_spread={sp:.2f} outside_spread={'yes' if abs(st.mean(d))>sp else 'no'}"
              f" | qps%: " + " ".join(f"{v:+.1f}" for v in dq) + f" mean={st.mean(dq):+.1f}%")
print("== Record share A - A0")
for cn in cells:
    a = st.mean(get(cn, "A", "cpu_us_per_stmt")); a0 = st.mean(get(cn, "A0", "cpu_us_per_stmt"))
    print(f"{cn} A-A0={a-a0:+.2f} us = {100*(a-a0)/a:.1f}% of A's CPU/stmt")
print("== loads (loadavg at end of each lifetime)")
for cn in cells:
    print(cn, " ".join(f"{r['load']}" for r in rows if r["cn"] == cn))
