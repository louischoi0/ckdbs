import sys
sys.path.insert(0, "/home/cdkbs/bench-run")
from an import *
from s1_tables import sweep

print("Scenario 1, `cores = 1`, `group`: planned (default) against `join_order = written`, 3 runs each, medians.\n")
print("| measure | planned | spread | written | spread | planned over written |")
print("|---|---|---|---|---|---|")
P = [new(f"pl-s1-planned-r{i}") for i in (1, 2, 3)]
W = [new(f"pl-s1-written-r{i}") for i in (1, 2, 3)]
for name in ("backtest-read", "backtest-replay", "compare-all", "compare-one"):
    a = [ph(d, name)["qps"] for d in P]
    b = [ph(d, name)["qps"] for d in W]
    print(f"| phase {name} | {f1(med(a), ' qps')} | {sp(spread(a))} | {f1(med(b), ' qps')} | {sp(spread(b))} | {ratio(med(a), med(b))} |")
for sh in ("cross-join", "point-join", "model-join"):
    for key in ("cold", "warm", "cabin"):
        a = [sweep(d)[sh][key]["qps"] for d in P if sweep(d)[sh].get(key)]
        b = [sweep(d)[sh][key]["qps"] for d in W if sweep(d)[sh].get(key)]
        if a:
            print(f"| sweep {sh} {key} | {f1(med(a), ' qps')} | {sp(spread(a))} | {f1(med(b), ' qps')} | {sp(spread(b))} | {ratio(med(a), med(b))} |")
print()
print("Scenario 3, `cores = 1`, `group`, 10,000 loans: planned against `join_order = written`.\n")
print("| cell | shape | planned | spread | written | spread | planned over written |")
print("|---|---|---|---|---|---|---|")
for mode, rr in (("none", (1, 2, 3)), ("all", (1,))):
    P = [new(f"pl-s3-planned-{mode}-r{i}") for i in rr]
    W = [new(f"pl-s3-written-{mode}-r{i}") for i in rr]
    for sh in ("join-loan-user", "join-no-literal", "exists-correlated"):
        a = [1e6 / ph(d, sh)["mean_us"] for d in P]
        b = [1e6 / ph(d, sh)["mean_us"] for d in W]
        print(f"| n=10,000 {mode} ({len(rr)} runs) | {sh} | {f0(med(a), ' qps')} | {sp(spread(a))} | {f0(med(b), ' qps')} | {sp(spread(b))} | {ratio(med(a), med(b))} |")
print()
