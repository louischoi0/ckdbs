import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import analyze as A

res = A.J("gain2.json", [])
g = A.group(res)
print("| Cell | Arm | Ops | CPU us/stmt | QPS | p0 | p25 | p50 | p95 | p99 | spread |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
S = {}
for cell, arms in g.items():
    for arm in ("A", "A2", "B", "BL"):
        s = A.arm_stats(arms[arm]); S[(cell, arm)] = s
        print(f"| {cell} | {arm} | {arms[arm][0]['ops']} | {s['cpu_mean']:.2f} | {s['qps']:,.0f} | {s['p0']:.0f} | {s['p25']:.0f} | {s['p50']:.0f} | {s['p95']:.0f} | {s['p99']:.0f} | {s['spread']:.1f}% |")
print()
for cell, arms in g.items():
    f = lambda a, b: A.d(S[(cell, a)]["cpu_mean"], S[(cell, b)]["cpu_mean"])
    print(cell, f"B vs A {f('A','B'):+.1f}%  BL vs A {f('A','BL'):+.1f}%  A2 vs A {f('A','A2'):+.1f}%  B vs BL {f('BL','B'):+.1f}%")
m = A.J("gain2-meta.json", {})
for k, v in m.items(): print(k, {a: b for a, b in v.items() if a.startswith("plan") or a.startswith("pool_")})
