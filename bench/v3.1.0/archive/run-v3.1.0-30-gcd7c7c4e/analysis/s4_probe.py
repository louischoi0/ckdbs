import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

for lab, d in (("A", load("s4")), ("B", load("cd2-s4")), ("oldA", load("s4", 4)), ("oldB", load("cd2-s4", 4))):
    for day in (1, 2, 3):
        row = []
        for arm in ("off", "on", "declared"):
            p = ph(d, f"d{day}-board[{arm}]")
            row.append(f"{arm}: mean {p['mean_us']:.0f} p50 {p['p50_us']:.0f} p95 {p['p95_us']:.0f} p99 {p['p99_us']:.0f}")
        print(lab, day, " | ".join(row))
print()
# walk cost per row: tape10k vs tape200 off p50
for lab, d in (("now A", load("s4")), ("then A", load("s4", 4))):
    a = ph(d, "d1-tape200[off]")["p50_us"]
    b = ph(d, "d1-tape10k[off]")["p50_us"]
    c = ph(d, "d1-tape1k[off]")["p50_us"]
    print(lab, "tape off p50 200/1k/10k", a, c, b, "ns/row (10k-200)/9800 =", 1000 * (b - a) / 9800)
print()
for lab, d in (("A", load("s4")), ("B", load("cd2-s4"))):
    print(lab, "elapsed", d["meta"]["elapsed_s"], "client_errors", d["meta"]["client_errors"], "slot_overruns", d["meta"]["slot_overruns"])
    print(" verify", [(v.get("mismatches"), v.get("n", None)) for v in d["verify"]][:3])
print(load("s4")["verify"][0].keys())
# share of probes walking: on arm board fraction with latency > 400 us is not available; use cpu seconds
# board throughput ratio on d3: A vs B
for lab, d in (("now", (load("s4"), load("cd2-s4"))), ("then", (load("s4", 4), load("cd2-s4", 4)))):
    a = 1e6 / ph(d[0], "d3-board[on]")["mean_us"]
    b = 1e6 / ph(d[1], "d3-board[on]")["mean_us"]
    print(lab, "d3 board on A", a, "B", b, "B/A", b / a)
# device thermometer: open p50 and load p50
for lab, d in (("A", load("s4")), ("B", load("cd2-s4"))):
    print(lab, "load p50", [ph(d, f"load[{a}]")["p50_us"] for a in ("off", "on", "declared")], "d1 open p50", [ph(d, f"d1-open[{a}]")["p50_us"] for a in ("off", "on", "declared")])
