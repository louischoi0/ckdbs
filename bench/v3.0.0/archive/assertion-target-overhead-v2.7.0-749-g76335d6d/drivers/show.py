import json, sys
m = json.load(open("/home/cdkbs/bench-runs/at/res/analysis.json"))["cells"]
s = json.load(open("/home/cdkbs/bench-runs/at/res-swap/analysis.json"))["cells"]
print("| cell | A p50 | B p50 | d p50 med [q1,q3] B>A | d p99 med | d mean med | swap d p50 med | x (binary) p50 | s (slot) p50 | x % of A | d engine us/op | swap d eng | x eng |")
for k in m:
    c, w = m[k], s[k]
    dm, ds = c["d_p50"]["median"], w["d_p50"]["median"]
    x, sl = (dm - ds) / 2, (dm + ds) / 2
    em, es = c["d_engine_us_per_op"]["median"] if "d_engine_us_per_op" in c else float("nan"), w["d_engine_us_per_op"]["median"] if "d_engine_us_per_op" in w else float("nan")
    print(f"| {k} | {c['A_p50']:.1f} | {c['B_p50']:.1f} | {dm:+.2f} [{c['d_p50']['q1']:+.2f},{c['d_p50']['q3']:+.2f}] {c['d_p50']['B_gt_A']}/{c['d_p50']['n']} "
          f"| {c['d_p99']['median']:+.2f} | {c['d_mean']['median']:+.2f} | {ds:+.2f} [{w['d_p50']['q1']:+.2f},{w['d_p50']['q3']:+.2f}] {w['d_p50']['B_gt_A']}/{w['d_p50']['n']} "
          f"| {x:+.2f} | {sl:+.2f} | {100*x/c['A_p50']:+.2f}% | {em:+.2f} | {es:+.2f} | {(em-es)/2:+.2f} |")
