import json, os, statistics, sys
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import analyze as A

O = A.OUT
res = A.J("extra.json", [])
g = A.group(res)
print("### same plan, two engines (join_order=written on A and B binaries; BP = B planned)\n")
print("| Cell | Arm | Ops/block | CPU us/stmt | QPS | p0 | p25 | p50 | p95 | p99 | Round spread |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
S = {}
for cell, arms in g.items():
    for arm in ("WA:drv", "WB:drv", "WA:swap", "WB:swap", "BP:drv"):
        if arm not in arms: continue
        s = A.arm_stats(arms[arm]); S[(cell, arm)] = s
        print(f"| {cell} | {arm} | {arms[arm][0]['ops']} | {s['cpu_mean']:.2f} | {s['qps']:,.0f} | {s['p0']:.0f} | {s['p25']:.0f} | {s['p50']:.0f} | {s['p95']:.0f} | {s['p99']:.0f} | {s['spread']:.1f}% |")
print("\n| Cell | WB:drv vs WA:drv (same driver plan) | WB:swap vs WA:swap (same swapped plan) | WA:swap vs WA:drv (plan effect, A code) | BP:drv vs WB:swap (planning cost on B) | BP:drv vs WA:drv (realised flip) |")
print("|---|---|---|---|---|---|")
for cell in g:
    f = lambda a, b: A.d(S[(cell, a)]["cpu_mean"], S[(cell, b)]["cpu_mean"])
    print(f"| {cell} | {f('WA:drv','WB:drv'):+.1f}% | {f('WA:swap','WB:swap'):+.1f}% | {f('WA:drv','WA:swap'):+.1f}% | {f('WB:swap','BP:drv'):+.1f}% | {f('WA:drv','BP:drv'):+.1f}% |")
print()
if os.path.exists(O + "/extra-plans.json"):
    for c, v in A.J("extra-plans.json").items():
        print(c)
        for k, l in v.items(): print("  ", k, l[0][:100], "|", l[1], "|", l[2][:90])

res = A.J("extra-uniq.json", [])
g = A.group(res)
print("\n### memo miss: a unique literal per execution (always-true conjunct), A planned vs B planned\n")
print("| Cell | Arm | Ops/block | CPU us/stmt | QPS | p0 | p25 | p50 | p95 | p99 | Round spread |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
S = {}
for cell, arms in g.items():
    for arm in ("PA:uniq", "PA2:uniq", "BP:uniq"):
        if arm not in arms: continue
        s = A.arm_stats(arms[arm]); S[(cell, arm)] = s
        print(f"| {cell} | {arm} | {arms[arm][0]['ops']} | {s['cpu_mean']:.2f} | {s['qps']:,.0f} | {s['p0']:.0f} | {s['p25']:.0f} | {s['p50']:.0f} | {s['p95']:.0f} | {s['p99']:.0f} | {s['spread']:.1f}% |")
print("\n| Cell | BP vs PA: CPU | PA2 vs PA (floor): CPU | BP vs PA: p50 | BP vs PA: p99 | per-round BP vs PA |")
print("|---|---|---|---|---|---|")
for cell, arms in g.items():
    a, c, t = S[(cell, "PA:uniq")], S[(cell, "PA2:uniq")], S[(cell, "BP:uniq")]
    ra = {b["round"]: b for b in arms["PA:uniq"]}; rt = {b["round"]: b for b in arms["BP:uniq"]}
    rd = [A.d(ra[k]["server_cpu_us"], rt[k]["server_cpu_us"]) for k in sorted(ra)]
    print(f"| {cell} | {A.d(a['cpu_mean'], t['cpu_mean']):+.1f}% | {A.d(a['cpu_mean'], c['cpu_mean']):+.1f}% | {A.d(a['p50'], t['p50']):+.1f}% | {A.d(a['p99'], t['p99']):+.1f}% | {', '.join(f'{x:+.1f}' for x in rd)} |")
if os.path.exists(O + "/extra-uniq-plans.json"):
    for c, v in A.J("extra-uniq-plans.json").items():
        print(c, v["PA"][2][:80], "||", v["BP"][2][:120])
m = A.J("extra-meta-BP.json", {})
print({k: v for k, v in m.items() if k.startswith("plan_memo") or k.startswith("plan_stats")})
