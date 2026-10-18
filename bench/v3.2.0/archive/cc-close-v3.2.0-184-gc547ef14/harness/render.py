#!/usr/bin/env python3
"""Markdown fragments with units in every cell. usage: render.py <outdir> <phase> <arms comma list> [base ctrl test]"""
import json, os, statistics, sys

sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")


def pct(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p / 100
    f = int(k); c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def d(a, b): return (b - a) / a * 100


def load(out, phase): return json.load(open(f"{out}/{phase}.json"))


def stats(blocks):
    lat = [x for b in blocks for x in b["lat_us"]]
    cpu = [b["server_cpu_us"] for b in sorted(blocks, key=lambda b: b["round"])]
    return {"n": len(lat), "cpu": statistics.mean(cpu), "rounds": cpu, "qps": statistics.mean(b["qps"] for b in blocks),
            "p0": min(lat), "p25": pct(lat, 25), "p50": pct(lat, 50), "p95": pct(lat, 95), "p99": pct(lat, 99),
            "spread": (max(cpu) - min(cpu)) / statistics.mean(cpu) * 100}


def groups(res):
    g = {}
    for r in res: g.setdefault(r["cell"], {}).setdefault(r["arm"], []).append(r)
    return g


def fmt_us(x): return f"{x:,.1f} µs" if x < 100 else f"{x:,.0f} µs"


def dist_table(g, arms, label_cell="Cell"):
    out = [f"| {label_cell} | Arm | Statements (n) | Server CPU / statement | Throughput | p0 | p25 | p50 | p95 | p99 | Round spread (CPU) |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    S = {}
    for cell, a in g.items():
        for arm in arms:
            if arm not in a: continue
            s = stats(a[arm]); S[(cell, arm)] = s
            out.append(f"| {cell} | {arm} | {s['n']:,} | {s['cpu']:,.2f} µs | {s['qps']:,.0f} stmt/s | {fmt_us(s['p0'])} | {fmt_us(s['p25'])} | {fmt_us(s['p50'])} | {fmt_us(s['p95'])} | {fmt_us(s['p99'])} | {s['spread']:.1f} % |")
    return "\n".join(out), S


def delta_table(g, S, base, ctrl, test, floor_min=2.0):
    out = [f"| Cell | {test} vs {base}: CPU | {test} vs {base}: throughput | {ctrl} vs {base} (noise floor): CPU | {test} vs {ctrl}: CPU | {test} vs {base}: p50 / p99 | Per-round {test} vs {base} CPU | Floor used | Verdict |",
           "|---|---|---|---|---|---|---|---|---|"]
    for cell, a in g.items():
        if (cell, base) not in S or (cell, test) not in S: continue
        b, c, t = S[(cell, base)], S[(cell, ctrl)], S[(cell, test)]
        ra = {x["round"]: x for x in a[base]}; rt = {x["round"]: x for x in a[test]}
        rd = [d(ra[k]["server_cpu_us"], rt[k]["server_cpu_us"]) for k in sorted(ra) if k in rt]
        floor = max(abs(d(b["cpu"], c["cpu"])), b["spread"] / 2, floor_min)
        dt = d(b["cpu"], t["cpu"])
        verdict = "within floor" if abs(dt) <= floor else ("faster" if dt < 0 else "slower")
        out.append(f"| {cell} | {dt:+.1f} % | {d(b['qps'], t['qps']):+.1f} % | {d(b['cpu'], c['cpu']):+.1f} % | {d(c['cpu'], t['cpu']):+.1f} % | {d(b['p50'], t['p50']):+.1f} % / {d(b['p99'], t['p99']):+.1f} % | {', '.join(f'{x:+.1f}' for x in rd)} % | ±{floor:.1f} % | {verdict} |")
    return "\n".join(out)


if __name__ == "__main__":
    out, phase, arms = sys.argv[1], sys.argv[2], sys.argv[3].split(",")
    g = groups(load(out, phase))
    t, S = dist_table(g, arms)
    print(t)
    if len(sys.argv) > 4:
        print()
        print(delta_table(g, S, sys.argv[4], sys.argv[5], sys.argv[6]))
