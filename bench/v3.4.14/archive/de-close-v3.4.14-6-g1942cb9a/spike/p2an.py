import json
import os
import statistics as st
import sys

d = json.load(open(os.path.expanduser(sys.argv[1])))
runs = d["runs"]
base = sys.argv[2] if len(sys.argv) > 2 else "A"
# server_cpu_us per statement, by (shape, path, bin), per lifetime.
table = {}
for li, r in enumerate(runs):
    for c in r["arms"]:
        table.setdefault((c["shape"], c["path"]), {}).setdefault(c["bin"], []).append(c["server_cpu_us"])
print(f"server CPU us per statement; delta against {base}, per lifetime, then median")
for key in sorted(table):
    arms = table[key]
    print(key, base, [round(x, 2) for x in arms[base]])
    for b in sorted(arms):
        if b == base:
            continue
        deltas = [x - y for x, y in zip(arms[b], arms[base])]
        print(f"   {b:3} - {base}: " + " ".join(f"{x:+.2f}" for x in deltas) + f"   median {st.median(deltas):+.2f}")
