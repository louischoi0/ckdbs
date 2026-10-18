#!/usr/bin/env python3
"""Summaries for the CC-close results file: prints markdown tables from out/*.json."""
import json, os, statistics, sys

OUT = os.environ.get("CC_OUT", "/home/cdkbs/bench-runs/cc-close/out")


def J(n, d=None):
    p = f"{OUT}/{n}"
    return json.load(open(p)) if os.path.exists(p) else d


def pct(xs, p):
    xs = sorted(xs)
    if not xs: return float("nan")
    k = (len(xs) - 1) * p / 100
    f = int(k); c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def arm_stats(blocks):
    lat = [x for b in blocks for x in b["lat_us"]]
    cpu = [b["server_cpu_us"] for b in sorted(blocks, key=lambda b: b["round"])]
    return {"n": len(lat), "cpu_mean": statistics.mean(cpu), "cpu_rounds": cpu,
            "qps": statistics.mean([b["qps"] for b in blocks]),
            "p0": min(lat), "p25": pct(lat, 25), "p50": pct(lat, 50), "p95": pct(lat, 95), "p99": pct(lat, 99),
            "spread": (max(cpu) - min(cpu)) / statistics.mean(cpu) * 100}


def group(res):
    g = {}
    for r in res:
        g.setdefault(r["cell"], {}).setdefault(r["arm"], []).append(r)
    return g


def d(a, b): return (b - a) / a * 100


def table(name, arms_order, base="A", ctrl="A2", test="B", pair_save=False):
    res = J(name + ".json", [])
    g = group(res)
    print(f"\n### {name}: server CPU per statement and latency\n")
    print("| Cell | Arm | Ops/block | CPU us/stmt (mean of rounds) | QPS | p0 us | p25 us | p50 us | p95 us | p99 us | Round spread |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    summ = {}
    for cell, arms in g.items():
        for arm in arms_order:
            if arm not in arms: continue
            s = arm_stats(arms[arm]); summ[(cell, arm)] = s
            ops = arms[arm][0]["ops"]
            print(f"| {cell} | {arm} | {ops} | {s['cpu_mean']:.2f} | {s['qps']:,.0f} | {s['p0']:.0f} | {s['p25']:.0f} | {s['p50']:.0f} | {s['p95']:.0f} | {s['p99']:.0f} | {s['spread']:.1f}% |")
    return g, summ


def delta_table(g, summ, base, ctrl, test, title):
    print(f"\n### {title}\n")
    print(f"| Cell | {test} vs {base}: CPU | {ctrl} vs {base} (floor): CPU | {test} vs {base}: p50 | {ctrl} vs {base}: p50 | {test} vs {base}: p99 | {ctrl} vs {base}: p99 | per-round {test} vs {base} CPU | Verdict |")
    print("|---|---|---|---|---|---|---|---|---|")
    for cell, arms in g.items():
        if (cell, base) not in summ or (cell, test) not in summ: continue
        a, c, t = summ[(cell, base)], summ[(cell, ctrl)], summ[(cell, test)]
        rd = []
        ra = {b["round"]: b for b in arms[base]}; rt = {b["round"]: b for b in arms[test]}
        for k in sorted(ra):
            if k in rt: rd.append(d(ra[k]["server_cpu_us"], rt[k]["server_cpu_us"]))
        floor_c = abs(d(a["cpu_mean"], c["cpu_mean"]))
        floor = max(floor_c, a["spread"] / 2)
        dt = d(a["cpu_mean"], t["cpu_mean"])
        verdict = "within floor" if abs(dt) <= max(floor, 2.0) else ("B faster" if dt < 0 else "B slower")
        print(f"| {cell} | {dt:+.1f}% | {d(a['cpu_mean'], c['cpu_mean']):+.1f}% | {d(a['p50'], t['p50']):+.1f}% | {d(a['p50'], c['p50']):+.1f}% | {d(a['p99'], t['p99']):+.1f}% | {d(a['p99'], c['p99']):+.1f}% | {', '.join(f'{x:+.1f}' for x in rd)} | {verdict} (floor max(|ctrl|, spread/2, 2%) = {max(floor, 2.0):.1f}%) |")


if __name__ == "__main__":
    which = sys.argv[1:] or ["overhead", "gain", "memo", "cold"]
    if "overhead" in which:
        g, s = table("overhead", ["A", "A2", "B"])
        delta_table(g, s, "A", "A2", "B", "overhead: deltas and noise floor")
    if "gain" in which:
        g, s = table("gain", ["A", "A2", "B"])
        delta_table(g, s, "A", "A2", "B", "gain: deltas and noise floor")
    if "memo" in which:
        g, s = table("memo", ["A-same", "A2-same", "B-same", "A-vary", "B-vary"])
        for lbl, (b, c, t) in {"same literal": ("A-same", "A2-same", "B-same")}.items():
            delta_table(g, s, b, c, t, f"memo: {lbl}")
        print("\n(vary) B-vary vs A-vary:")
        for cell, arms in g.items():
            if ("A-vary" in arms and "B-vary" in arms):
                a, t = arm_stats(arms["A-vary"]), arm_stats(arms["B-vary"])
                print(f"- {cell}: CPU {d(a['cpu_mean'], t['cpu_mean']):+.1f}%  (A-vary {a['cpu_mean']:.2f}, B-vary {t['cpu_mean']:.2f} us)")
    if "cold" in which:
        cold = J("cold.json", [])
        cells = []
        for r in cold:
            if r["cell"] not in cells: cells.append(r["cell"])
        print("\n### cold: first executions on a fresh server (A data-free by construction; B before any publish)\n")
        print("| Cell | Server | 1st stmt CPU us | 1st stmt latency us | 2nd | 3rd | mean of 2nd-20th CPU us | median of 2nd-20th latency us |")
        print("|---|---|---|---|---|---|---|---|")
        for c in cells:
            for lab in ("A", "B"):
                rs = [r for r in cold if r["cell"] == c and r["server"] == lab]
                rs.sort(key=lambda r: r["i"])
                rest = rs[1:]
                print(f"| {c} | {lab} | {rs[0]['cpu_us']:.0f} | {rs[0]['lat_us']:.0f} | {rs[1]['lat_us']:.0f} | {rs[2]['lat_us']:.0f} | {statistics.mean(r['cpu_us'] for r in rest):.1f} | {statistics.median(r['lat_us'] for r in rest):.0f} |")
