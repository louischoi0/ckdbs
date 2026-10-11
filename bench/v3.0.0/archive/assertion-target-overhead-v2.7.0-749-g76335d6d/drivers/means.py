import json
R = "/home/cdkbs/bench-runs/at"
m = json.load(open(f"{R}/res/analysis.json"))["cells"]
s = json.load(open(f"{R}/res-swap/analysis.json"))["cells"]
for k in m:
    c, w = m[k], s[k]
    print(k, "A mean", round(c["A_mean"], 1), "B mean", round(c["B_mean"], 1), "d mean main", round(c["d_mean"]["median"], 2),
          [round(x, 1) for x in c["d_mean"]["all"]], "swap", round(w["d_mean"]["median"], 2), [round(x, 1) for x in w["d_mean"]["all"]])
