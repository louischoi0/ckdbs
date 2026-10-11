import json, sys
for d in sys.argv[1:]:
    a = json.load(open(f"{d}/analysis.json"))["cells"]
    print("==", d)
    for k, c in a.items():
        n, arm = k.split("|")

        def f(key):
            x = c["d_" + key]
            return f"{x['median']:+7.2f} [{x['q1']:+6.2f},{x['q3']:+6.2f}] {x['B_gt_A']}/{x['n']}"
        print(f"{n:>6} {arm:<11} A p50 {c['A_p50']:7.1f} B {c['B_p50']:7.1f} | dp50 {f('p50')} | dp99 {f('p99')} | dmean {f('mean')} | deng {f('engine_us_per_op')} | A_eng {c['A_engine_us_per_op']:.2f}")
