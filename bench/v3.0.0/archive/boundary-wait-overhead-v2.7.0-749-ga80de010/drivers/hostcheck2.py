import json, glob
res = "/home/cdkbs/bench-runs/bw/res/"
over = []
for f in sorted(glob.glob(res + "*/*.host.json")):
    h = json.load(open(f))
    name = f[len(res):]
    pin = ("6", "7") if name.startswith("c1") else ("3", "4", "5", "6", "7")
    for c, v in h["cpu_busy_max_1s"].items():
        if c not in pin and v > 0.5:
            over.append((name, c, v, h["samples"], h["start_utc"]))
print(len(over))
for o in over:
    print(o)
