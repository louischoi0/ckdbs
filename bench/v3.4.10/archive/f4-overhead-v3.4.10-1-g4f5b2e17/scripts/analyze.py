#!/usr/bin/env python3
"""Per cell: medians over runs per arm (A, B, and A2 = a second server of A's binary, the noise control):
server CPU per statement with [min,max] over runs, B-A and A2-A deltas, p0/p25/p50/p75/p99, QPS=1e6/mean."""
import json, statistics as st, sys
r = json.load(open(sys.argv[1]))
cells = {}
for x in r:
    cells.setdefault((x["cell"], x["rows"], x["shape"]), {}).setdefault(x["bin"], []).append(x)
med = st.median
print("server CPU per statement (us): median [min,max] over runs; deltas of medians; per-run B-A sign count")
for k, d in cells.items():
    cpu = {l: [x["server_cpu_us"] for x in d[l]] for l in d}
    a = med(cpu["A"])
    dB = med(cpu["B"]) - a
    dC = med(cpu["A2"]) - a
    nneg = sum(1 for p, q in zip(cpu["A"], cpu["B"]) if q < p)
    nnegc = sum(1 for p, q in zip(cpu["A"], cpu["A2"]) if q < p)
    print(k, "ops/run", d["A"][0]["ops"],
          "| A %.2f [%.2f,%.2f]" % (a, min(cpu["A"]), max(cpu["A"])),
          "| A2 %.2f [%.2f,%.2f]" % (med(cpu["A2"]), min(cpu["A2"]), max(cpu["A2"])),
          "| B %.2f [%.2f,%.2f]" % (med(cpu["B"]), min(cpu["B"]), max(cpu["B"])),
          "| B-A %+.2f us (%+.1f%%, %+.1f ns/row) B<A in %d/%d runs" % (dB, 100 * dB / a, dB * 1000 / k[1], nneg, len(cpu["A"])),
          "| A2-A %+.2f us (%+.1f%%) A2<A in %d/%d" % (dC, 100 * dC / a, nnegc, len(cpu["A"])),
          "| bad", sum(x["bad"] for l in d for x in d[l]))
print()
print("latency (us), median over runs")
for k, d in cells.items():
    for lab in ("A", "A2", "B"):
        xs = d[lab]
        f = lambda key: med([x[key] for x in xs])
        print(k, lab, "p0 %.1f p25 %.1f p50 %.1f p75 %.1f p99 %.1f mean %.1f qps %.0f" % (
            f("p0_us"), f("p25_us"), f("p50_us"), f("p75_us"), f("p99_us"), f("mean_us"), 1e6 / f("mean_us")))
