import json
B = "/home/cdkbs/bench-runs/lw"
for d, sizes in (("res", ("200", "1000", "10000")), ("res-swap", ("1000",)), ("res-swap10k", ("10000",)),
                 ("res-ctl", ("1000", "10000")), ("res-ctl-swap", ("10000",))):
    a = json.load(open(f"{B}/{d}/analysis.json"))["cells"]
    print(d)
    for k, c in a.items():
        n, arm = k.split("|")
        if n in sizes and arm in ("sel-pk", "sel-pk-long", "upd-pk", "del-pk", "ctl-insert", "ctl-ping", "sel-nonpk"):
            e = c["d_engine_us_per_op"]
            print(f"  {n:>6} {arm:<12} engine/op A {c['A_engine_us_per_op']:.2f} B {c['B_engine_us_per_op']:.2f} d {e['median']:+.3f} ({e['q1']:+.3f},{e['q3']:+.3f}) B>A {e['B_gt_A']}/{e['n']}")
