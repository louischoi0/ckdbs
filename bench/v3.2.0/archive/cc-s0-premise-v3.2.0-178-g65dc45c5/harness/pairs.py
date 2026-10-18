import json, statistics, sys
R = json.load(open(sys.argv[1]))
cells = list(dict.fromkeys(r["cell"] for r in R))
cpu = {(r["cell"], r["round"], r["arm"]): r["server_cpu_us"] for r in R}
print("per-pair saving of the alternative order W:B against the planned pick P:A and against W:A (same round), and the control W2:A vs W:A")
for c in cells:
    if (c, 1, "W:B") not in cpu:
        # c3: search cost
        d = [cpu[(c, k, "P:A")] - cpu[(c, k, "W:A")] for k in (1, 2, 3)]
        dc = [cpu[(c, k, "W2:A")] - cpu[(c, k, "W:A")] for k in (1, 2, 3)]
        print(f"{c}: P:A - W:A per pair (us) {[round(x,1) for x in d]} median {statistics.median(d):.1f}; control W2-W {[round(x,1) for x in dc]}")
        continue
    sp = [(cpu[(c, k, "P:A")] - cpu[(c, k, "W:B")]) / cpu[(c, k, "P:A")] * 100 for k in (1, 2, 3)]
    sw = [(cpu[(c, k, "W:A")] - cpu[(c, k, "W:B")]) / cpu[(c, k, "W:A")] * 100 for k in (1, 2, 3)]
    dc = [(cpu[(c, k, "W2:A")] / cpu[(c, k, "W:A")] - 1) * 100 for k in (1, 2, 3)]
    dpp = [(cpu[(c, k, "P:A")] / cpu[(c, k, "W:A")] - 1) * 100 for k in (1, 2, 3)]
    dpb = [(cpu[(c, k, "P:B")] / cpu[(c, k, "W:A")] - 1) * 100 for k in (1, 2, 3)]
    print(f"{c}: save vs P:A {[round(x,1) for x in sp]} min {min(sp):.1f}; save vs W:A {[round(x,1) for x in sw]} min {min(sw):.1f}; "
          f"control {[round(x,1) for x in dc]}; P:A vs W:A {[round(x,1) for x in dpp]}; P:B vs W:A {[round(x,1) for x in dpb]}")
