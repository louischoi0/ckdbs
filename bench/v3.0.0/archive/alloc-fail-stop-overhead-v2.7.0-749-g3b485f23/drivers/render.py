import json
R = "/home/cdkbs/bench-runs/af"
m = json.load(open(f"{R}/res/analysis.json"))["cells"]
s = json.load(open(f"{R}/res-swap/analysis.json"))["cells"]
names = {"1000|select-pk": "1 pk point SELECT (1000 rows)",
         "1000|select-pk-again": "1c pk point SELECT, repeated (noise floor)",
         "1000|insert": "2 single-row INSERT (1000-row base)",
         "1000|insert-again": "2c INSERT into t2, repeated (noise floor)",
         "1000|exists": "3 correlated EXISTS (200 rows)",
         "1000|join": "3b equi-join (200 rows)",
         "1000|ping": "4 SHOW META (control)"}
order = list(names)
print("## matrix")
print("| cell | A QPS | B QPS | B-A QPS | d p50 us (B-A) med [IQR] | B>A runs | swap d p50 med [IQR] (B>A) | binary effect x us | x % of A p50 |")
print("|---|---|---|---|---|---|---|---|---|")
for k in order:
    c, w = m[k], s[k]
    qa, qb = 1e6 / c["A_mean"], 1e6 / c["B_mean"]
    dm, ds = c["d_p50"]["median"], w["d_p50"]["median"]
    x = (dm - ds) / 2
    print(f"| {names[k]} | {qa:,.0f} | {qb:,.0f} | {qb-qa:+,.0f} | {dm:+.2f} [{c['d_p50']['q1']:+.2f}, {c['d_p50']['q3']:+.2f}] | {c['d_p50']['B_gt_A']}/{c['d_p50']['n']} | {ds:+.2f} [{w['d_p50']['q1']:+.2f}, {w['d_p50']['q3']:+.2f}] ({w['d_p50']['B_gt_A']}/{w['d_p50']['n']}) | {x:+.2f} | {100*x/c['A_p50']:+.2f}% |")
print("\n## dist (main pass; per-run percentile, median over 8 runs; us)")
print("| cell | engine | ops/run | p0 | p25 | p50 | p75 | p99 | engine us/op |")
print("|---|---|---|---|---|---|---|---|---|")
for k in order:
    c = m[k]
    n = 1600 if k.split("|")[1] in ("exists", "join") else 16000
    for lab, nm in (("A", "A"), ("B", "B")):
        print(f"| {names[k]} | {nm} | {n} | {c[lab+'_p0']:.1f} | {c[lab+'_p25']:.1f} | {c[lab+'_p50']:.1f} | {c[lab+'_p75']:.1f} | {c[lab+'_p99']:.1f} | {c[lab+'_engine_us_per_op']:.2f} |")
print("\n## d percentile deltas B-A (main med [q1,q3]; swap med)")
for k in order:
    c, w = m[k], s[k]
    parts = [f"{p} {c['d_'+p]['median']:+.2f} [{c['d_'+p]['q1']:+.2f},{c['d_'+p]['q3']:+.2f}] sw {w['d_'+p]['median']:+.2f}" for p in ("p0", "p25", "p50", "p75", "p99", "mean")]
    print(names[k], "|", " ; ".join(parts))
print("\n## per-run d p50")
for k in order:
    print(k, "main", [round(x, 2) for x in m[k]["d_p50"]["all"]], "swap", [round(x, 2) for x in s[k]["d_p50"]["all"]])
print("\n## engine us/op (sched_foreground_polled_us / polls)")
for k in order:
    c, w = m[k], s[k]
    print(k, "A", round(c["A_engine_us_per_op"], 2), "B", round(c["B_engine_us_per_op"], 2), "d", round(c["d_engine_us_per_op"]["median"], 2), "[%+.2f,%+.2f]" % (c["d_engine_us_per_op"]["q1"], c["d_engine_us_per_op"]["q3"]), "swap d", round(w["d_engine_us_per_op"]["median"], 2))
