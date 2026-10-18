#!/usr/bin/env python3
"""Cross-replicate summaries. Replicates of the full matrix: out (R1) and out2 (R2); overhead2/overhead3 live in out."""
import json, os, re, statistics, sys

sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import render as R

B = "/home/cdkbs/bench-runs/cc-close"


def J(path, default=None):
    return json.load(open(path)) if os.path.exists(path) else default


def pooled_overhead():
    reps = [("R1 (5 rounds)", f"{B}/out", "overhead"), ("R-ov2 (10 rounds)", f"{B}/out", "overhead2"),
            ("R-ov3 (16 rounds)", f"{B}/out", "overhead3"), ("R2 (5 rounds)", f"{B}/out2", "overhead")]
    cells = ["ov-point-read", "ov-join3-tuplekey", "ov-insert", "ov-update"]
    print("| Cell | Replicate | Rounds | A | A2 | B | B vs A | A2 vs A | B vs A2 |")
    print("|---|---|---|---|---|---|---|---|---|")
    allp = {c: {"BA": [], "A2A": [], "BA2": []} for c in cells}
    for c in cells:
        for name, out, ph in reps:
            res = J(f"{out}/{ph}.json")
            if res is None: continue
            g = R.groups(res).get(c)
            if g is None: continue
            S = {a: R.stats(g[a]) for a in ("A", "A2", "B")}
            ra = {x["round"]: x["server_cpu_us"] for x in g["A"]}
            r2 = {x["round"]: x["server_cpu_us"] for x in g["A2"]}
            rb = {x["round"]: x["server_cpu_us"] for x in g["B"]}
            for k in ra:
                allp[c]["BA"].append(R.d(ra[k], rb[k])); allp[c]["A2A"].append(R.d(ra[k], r2[k])); allp[c]["BA2"].append(R.d(r2[k], rb[k]))
            print(f"| {c} | {name} | {len(ra)} | {S['A']['cpu']:.2f} µs | {S['A2']['cpu']:.2f} µs | {S['B']['cpu']:.2f} µs | {R.d(S['A']['cpu'], S['B']['cpu']):+.1f} % | {R.d(S['A']['cpu'], S['A2']['cpu']):+.1f} % | {R.d(S['A2']['cpu'], S['B']['cpu']):+.1f} % |")
    print("\nPooled per-round paired deltas (every round of every replicate):\n")
    print("| Cell | Rounds pooled | B vs A: mean / median / min..max | A2 vs A (floor): mean / median / min..max | B vs A2: mean / median |")
    print("|---|---|---|---|---|")
    for c in cells:
        p = allp[c]
        f = lambda xs: f"{statistics.mean(xs):+.2f} % / {statistics.median(xs):+.2f} % / {min(xs):+.1f}..{max(xs):+.1f} %"
        print(f"| {c} | {len(p['BA'])} | {f(p['BA'])} | {f(p['A2A'])} | {statistics.mean(p['BA2']):+.2f} % / {statistics.median(p['BA2']):+.2f} % |")


def host_summary():
    print("\nHost state per block:\n")
    for name, out in (("R1", f"{B}/out"), ("R2", f"{B}/out2")):
        n = 0; mx = 0.0; foreign = 0
        for f in os.listdir(out):
            if f.endswith(".json") and f.split(".")[0] in ("overhead", "gain", "memo", "overhead2", "overhead3", "gain2", "extra", "extra-uniq"):
                for r in J(f"{out}/{f}"):
                    h = r.get("host")
                    if not h: continue
                    n += 1; mx = max(mx, float(h["loadavg1"])); foreign += 1 if h["foreign"] else 0
        print(f"- {name}: {n} blocks, max loadavg1 at a block start {mx:.2f}, blocks with a build/ctest process visible: {foreign}")


def stability(out):
    st = J(f"{out}/stability.json")
    print(f"\nStability ({out}):")
    print("  ops", st["ops"], "cpu/op", round(st["cpu_us_per_op"], 1), "windows", [w["ops"] for w in st["wins"]])
    for k in ("plan_stats_generation", "plan_stats_publishes", "plan_stats_seeds", "plan_stats_seed_pages", "plan_stats_posteriors", "plan_stats_relations", "plan_stats_part_drops", "plan_stats_steward_us_max", "plan_memo_hits", "plan_memo_misses"):
        print(f"  {k}: {st['meta_before'].get(k)} -> {st['meta_after'].get(k)}")
    by = {}
    for o in st["orders"]:
        order = []
        label = None
        for l in o["plan"]:
            m = re.match(r"step (\d+) (\w+) (\S+)", l)
            if m and "opens=" not in l: order.append(f"{m.group(1)}:{m.group(2)}:{m.group(3)}")
            if l.startswith("planned order="): label = l.split()[1]
        by.setdefault(o["cell"], []).append((o["t"], label, tuple(order)))
    for c, v in by.items():
        orders = {x[2] for x in v}
        print(f"  {c}: {len(orders)} distinct executed order(s) over {len(v)} ANALYZEs; labels {[x[1].split('=')[1] for x in v]}; gens "
              f"{sorted({int(re.search(r'gen=(\d+)', l).group(1)) for o in st['orders'] if o['cell']==c for l in o['plan'] if 'planned order' in l})}")
        print("     ", sorted(orders)[0])


if __name__ == "__main__":
    pooled_overhead()
    host_summary()
    stability(f"{B}/out")
    if os.path.exists(f"{B}/out2/stability.json"): stability(f"{B}/out2")
