import sys, json
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

P = "/tmp/claude-1000/-home-cdkbs-ckdbs/ff2cbc3f-cf1f-478c-a618-6507b57727ba/scratchpad/old/bench/v3.0.0/archive/scenario2-pg18-v2.7.0-545-gf2f1ee7/json"
old = [json.load(open(f"{P}/s2-kds-b1-c1-g-r{i}.json")) for i in (1, 2, 3)]
new = [load(f"{r}b1-s2-c1-g") for r in ("", "r2-", "r3-")]
pg = [json.load(open(f"{P}/s2-pg-on-r{i}.json")) for i in (1, 2, 3)]
for n in ("commit", "booking", "freight-insert", "cargo-lookup"):
    for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us"):
        print(n, k, "old %.1f new %.1f pg %.1f" % tuple(statistics.median([ph(d, n)[k] for d in ds]) for ds in (old, new, pg)))
# time series of device-bound figures
print()
for t in ("s2-c1-g", "r2-s2-c1-g", "r3-s2-c1-g", "b1-s2-c1-g", "r3-b1-s2-c1-g"):
    d = load(t)
    print(t, "commit p0 %.0f p50 %.0f" % (ph(d, "commit")["p0_us"], ph(d, "commit")["p50_us"]), "load-cargos p0 %.0f p50 %.0f" % (ph(d, "load-cargos")["p0_us"], ph(d, "load-cargos")["p50_us"]), precheck(t)[0])
for t in ("s3-c1-n200-none", "r2-s3-c1-n200-none", "r3-s3-c8-n10000-all"):
    d = load(t)
    print(t, "load p0 %.0f p50 %.0f" % (ph(d, "load")["p0_us"], ph(d, "load")["p50_us"]), precheck(t)[0])
for t in ("s1b-c1-g", "r2-s1b-c1-g", "r3-s1b-c1-g"):
    d = load(t)
    print(t, "result-insert p0 %.0f p50 %.0f" % (ph(d, "result-insert")["p0_us"], ph(d, "result-insert")["p50_us"]), "load-sessions p50 %.1f" % ph(d, "load-sessions")["p50_us"], precheck(t)[0])
for t in ("s4", "cd2-s4"):
    d = load(t)
    print(t, "open p0", [ph(d, f"d1-open[{a}]")["p0_us"] for a in ("off", "on", "declared")], precheck(t + "-off")[0])
