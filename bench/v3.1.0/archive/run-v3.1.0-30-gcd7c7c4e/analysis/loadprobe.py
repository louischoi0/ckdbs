import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *
for n in (200, 1000, 10000):
    for m in ("none", "all"):
        a = ph(load(f"s3-c1-n{n}-{m}"), "load")
        b = ph(load(f"s3-c1-n{n}-{m}", 3), "load")
        print(n, m, "now p0 %.0f p50 %.0f p95 %.0f p99 %.0f mean %.0f ops %d" % (a["p0_us"], a["p50_us"], a["p95_us"], a["p99_us"], a["mean_us"], a["ops"]),
              "| then p0 %.0f p50 %.0f p95 %.0f p99 %.0f mean %.0f" % (b["p0_us"], b["p50_us"], b["p95_us"], b["p99_us"], b["mean_us"]))
