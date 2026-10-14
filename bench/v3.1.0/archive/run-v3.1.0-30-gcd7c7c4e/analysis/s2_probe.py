import sys, json
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

print("p0 now / then (run 1)")
for c in ("c1-g", "c8-g", "c1-s", "c8-s"):
    d, o = load(f"s2-{c}"), load(f"s2-{c}", 2)
    for n in ("cargo-lookup", "freight-insert", "charge-insert", "commit", "booking"):
        print(c, n, d_ := ph(d, n)["p0_us"], ph(o, n)["p0_us"], "p50", ph(d, n)["p50_us"], ph(o, n)["p50_us"])
print()
print("b1 shares of booking mean (3 runs median) g/s")
names = ["cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert", "operation-update", "org-update", "commit"]
for c in ("c1-g", "c1-s"):
    for r in ("", "r2-", "r3-"):
        d = load(f"{r}b1-s2-{c}")
        t = ph(d, "booking")
        tot = t["mean_us"] * t["ops"]
        sh = [ph(d, n)["mean_us"] * ph(d, n)["ops"] / tot for n in names]
        print(c, r or "r1", " ".join(f"{n[:6]}={100*x:.1f}" for n, x in zip(names, sh)), f"unattr={100*(1-sum(sh)):.1f} booking mean={t['mean_us']:.0f}")
print()
print("server log counts (TXN_CONFLICT lines / row id= lines / btree descent lines)")
for r in ("", "r2-", "r3-"):
    for c in ("c1-g", "c8-g", "c1-s", "c8-s"):
        t = f"{r}s2-{c}"
        txt = open(f"{L}/{t}.serverlog").read().split("\n")
        print(t, sum("TXN_CONFLICT" in x for x in txt), sum("row id=" in x for x in txt), sum("btree descent" in x and "gave up" in x for x in txt))
print()
print("RR probe vs default medians")
for c in ("c1-g", "c8-g"):
    m = statistics.median([load(f"{r}s2-{c}")["meta"]["tps"] for r in ("", "r2-", "r3-")])
    rr = load(f"rr-s2-{c}")["meta"]["tps"]
    print(c, m, rr, 100 * (rr / m - 1))
print()
print("conflicts per run at c8-s: ", [load(f"{r}s2-c8-s")["meta"]["outcomes"]["conflicted"] for r in ("", "r2-", "r3-")],
      "old", [load(f"{r}s2-c8-s", 2)["meta"]["outcomes"]["conflicted"] for r in ("", "r2-", "r3-")])
print("verify failures default cells", [load(f"{r}s2-{c}")["meta"]["verify"]["failures"] for r in ("", "r2-", "r3-") for c in ("c1-g", "c8-g", "c1-s", "c8-s")])
print("tps c8-s runs", [load(f"{r}s2-c8-s")["meta"]["tps"] for r in ("", "r2-", "r3-")])
