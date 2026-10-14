import sys
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from mdlib import *

print("| bookers | durability | tps | committed | conflicted | verify failures | booking p50 | commit p50 | commit share of booking | load-cargos p50 (device thermometer) |")
for b in (1, 2, 4, 8, 16):
    for c in ("g", "s"):
        if b == 8:
            ds = [load(f"{r}s2-c8-{c}") for r in ("", "r2-", "r3-")]
            d = sorted(ds, key=lambda x: x["meta"]["tps"])[1]
        else:
            d = load(f"x-b{b}-s2-c8-{c}")
        m = d["meta"]
        bk, cm = ph(d, "booking"), ph(d, "commit")
        print(f"| {b} | {c} | {m['tps']:.1f} | {m['outcomes']['committed']} | {m['outcomes']['conflicted']} | {m['verify']['failures']} | {bk['p50_us']:.0f} | {cm['p50_us']:.0f} | {100*cm['mean_us']*cm['ops']/(bk['mean_us']*bk['ops']):.1f} % | {ph(d, 'load-cargos')['p50_us']:.0f} |")
