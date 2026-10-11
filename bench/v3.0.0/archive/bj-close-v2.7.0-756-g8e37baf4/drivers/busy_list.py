import json, glob, sys
thr = float(sys.argv[1]) if len(sys.argv) > 1 else 0.17
for c in (1, 2):
    for f in sorted(glob.glob(f"/home/cdkbs/bench-runs/bj-close/out-c{c}/c{c}-n*-r*.json")):
        j = json.load(open(f))
        sc = {2, 4} | ({3} if c == 2 else set())
        o = [v for k, v in j["cpu_busy"].items() if int(k) not in sc]
        m = sum(o) / len(o)
        if m > thr:
            print(f.split("/")[-1], round(m, 3), j["gate_wait_s"], j["gate_other_busy_before"])
