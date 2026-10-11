import json, sys
for res in ("res", "res-swap"):
    d = json.load(open(f"/home/cdkbs/bench-runs/at/{res}/n10000-run1.json"))
    for l in "BA":
        print(res, l, [round(x) for x in d["create_lat_us"][l]])
d = json.load(open("/home/cdkbs/bench-runs/at/res/n1000-run1.json"))
for l in "BA":
    print("n1000", l, [round(x) for x in d["create_lat_us"][l]])
