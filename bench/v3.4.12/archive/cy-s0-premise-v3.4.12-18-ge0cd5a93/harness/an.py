#!/usr/bin/env python3
"""Analysis of cy_s0.py output. usage: an.py results.json > tables.md"""
import json, re, statistics, sys
import gzip
_p = sys.argv[1]
R = json.load(gzip.open(_p) if _p.endswith(".gz") else open(_p))
PL = json.load(open((_p[:-3] if _p.endswith(".gz") else _p) + ".plans.json"))
# server CPU is stored divided by the PLANNED ops; a time-capped block completed fewer. Rescale to completed ops.
_planned = {}
for b in R["blocks"]:
    _planned[(b["cell"], b["round"])] = max(_planned.get((b["cell"], b["round"]), 0), b["ops"])
for b in R["blocks"]:
    b["cpu_us"] = b["cpu_us"] * _planned[(b["cell"], b["round"])] / b["ops"]
INI = {"models": "m", "model_results": "r", "freights": "f", "cargos": "c", "daily_stats": "t", "daily_bars": "b",
       "symbols": "s", "users": "u", "loans": "l"}


def order_of(lines):
    o = []
    for l in lines:
        m = re.match(r"step (\d+) (\w+) (\w+?)_\d+ AS", l)
        if m and "opens=" not in l and "examined=" not in l:
            o.append(INI[m.group(3)])
    return "-".join(o)


def pct(v, q):
    v = sorted(v); i = (len(v) - 1) * q / 100; lo = int(i); hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


def line(lines, key):
    for l in lines:
        if l.startswith(key): return l
    return ""


cells = []
for b in R["blocks"]:
    if b["cell"] not in cells: cells.append(b["cell"])
print("## Order, est and obs per cell (planner, blind vs informed)\n")
print("| cell | blind order | blind line | informed order | informed line | obs (rows per step) |\n|---|---|---|---|---|---|")
for c in cells:
    bl, inf = PL["blind"][c], PL["informed"][c]
    obs = ", ".join(f"{m[0]}/{m[1]}" for m in re.findall(r"examined=(\d+) matched=(\d+)", " ".join(l for l in inf if l.startswith("step") and "opens=" in l)))
    print(f"| {c} | {order_of(bl)} | {line(bl, 'planned')} | {order_of(inf)} | {line(inf, 'planned')} | examined/matched: {obs} |")
print("\n## Literal conjunct selectivity (COUNT with / without)\n")
print("| cell | conjunct | with | without | true sel | planner constant | error factor (const / true) |\n|---|---|---|---|---|---|---|")
for s in R["sel"]:
    ts = s["with"] / s["without"]; cst = 0.01 if "=" in s["conjunct"] and "BETWEEN" not in s["conjunct"] and "<=" not in s["conjunct"] and ">=" not in s["conjunct"] else (0.3 if ("<=" in s["conjunct"] or ">=" in s["conjunct"]) else 0.3)
    print(f"| {s['cell']} | {s['conjunct']} | {s['with']} | {s['without']} | {ts:.4f} | {cst} | {cst / ts:.3g} |")
print("\n## Latency percentiles and server CPU per arm (all rounds pooled; us)\n")
print("| cell | arm | ops/block | n | p0 | p25 | p50 | p95 | p99 | QPS (derived 1e6/mean) | server CPU us/op (mean of rounds) |\n|---|---|---|---|---|---|---|---|---|---|---|")
G = {}
for c in cells:
    arms = []
    for b in R["blocks"]:
        if b["cell"] == c and b["arm"] not in arms: arms.append(b["arm"])
    for a in arms:
        bs = [b for b in R["blocks"] if b["cell"] == c and b["arm"] == a]
        lat = [x for b in bs for x in b["lat_us"]]
        cpu = statistics.mean(b["cpu_us"] for b in bs)
        G[(c, a)] = {"p50": pct(lat, 50), "cpu": cpu, "bs": bs, "p50r": {b["round"]: pct(b["lat_us"], 50) for b in bs}, "cpur": {b["round"]: b["cpu_us"] for b in bs}}
        print(f"| {c} | {a} | {bs[0]['ops']} | {len(lat)} | " + " | ".join(f"{pct(lat, q):.0f}" for q in (0, 25, 50, 95, 99)) + f" | {1e6 / statistics.mean(lat):.0f} | {cpu:.0f} |")
print("\n## Gate per cell\n")
print("(a) p50 >= 15 % faster than the planner's informed order (P arm) and outside the noise floor; (b) server CPU >= 15 % lower in every round (>= 3 rounds). Noise floor = |p50(W2) - p50(W:first)| / p50(W:first), the two being the same configuration and spelling.\n")
print("| cell | planner informed order | best forced (p50) | best forced p50 us | P p50 us | forced-same-as-planner p50 us | delta p50 | noise floor | (a) | (b) rounds with CPU>=15% lower | (b) | blind order == informed order | blind-order forced p50 us |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for c in cells:
    po = order_of(PL["informed"][c]); bo = order_of(PL["blind"][c])
    forced = {a.split(":")[1]: G[(c, a)] for (cc, a) in G if cc == c and a.startswith("W:")}
    # forced arm names are first-letter orders e.g. r-m ; planner order e.g. m-r
    best = min(forced, key=lambda k: forced[k]["p50"])
    P = G[(c, "P:" + [a.split(":")[1] for (cc, a) in G if cc == c and a.startswith("P:")][0])]
    first = [a.split(":")[1] for (cc, a) in G if cc == c and a.startswith("W2:")][0]
    w2 = G[(c, "W2:" + first)]; wf = forced[first]
    noise = abs(w2["p50"] - wf["p50"]) / wf["p50"]
    same = forced.get(po, {}).get("p50")
    d = (P["p50"] - forced[best]["p50"]) / P["p50"]
    a_ok = best != po and d >= 0.15 and d > noise
    rr = [r for r in P["cpur"] if forced[best]["cpur"][r] <= 0.85 * P["cpur"][r]]
    b_ok = best != po and len(rr) == len(P["cpur"]) and len(rr) >= 3
    bl = forced.get(bo, {}).get("p50")
    print(f"| {c} | {po} | {best} | {forced[best]['p50']:.0f} | {P['p50']:.0f} | {same and round(same)} | {d * 100:.1f}% | {noise * 100:.1f}% | {'yes' if a_ok else 'no'} | {len(rr)}/{len(P['cpur'])} | {'yes' if b_ok else 'no'} | {'yes' if bo == po else 'NO'} | {bl and round(bl)} |")
print("\n## Host per block (max load1, foreign build processes seen)\n")
print(max(float(b["host"]["load1"]) for b in R["blocks"]), sum(b["host"]["foreign"] for b in R["blocks"]))
bad = [(b["cell"], b["round"]) for b in R["blocks"]]
rows = {}
for b in R["blocks"]: rows.setdefault((b["cell"], b["round"]), set()).add(b["rows"])
print("rows-per-cell-round equal across arms:", all(len(v) == 1 for v in rows.values()))

print("\n## Every forced order that differs from the planner's order (all rounds)\n")
print("| cell | forced order | rounds p50 >= 15 % below P | rounds CPU >= 15 % below P | per-round p50 us (P/forced) |\n|---|---|---|---|---|")
for c in cells:
    po = order_of(PL["informed"][c])
    pa = [a for (cc, a) in G if cc == c and a.startswith("P:")][0]
    for (cc, a) in sorted(G):
        if cc != c or not a.startswith("W:") or a.split(":")[1] == po: continue
        f = G[(c, a)]; P = G[(c, pa)]
        n50 = [r for r in P["p50r"] if f["p50r"][r] <= 0.85 * P["p50r"][r]]
        ncp = [r for r in P["cpur"] if f["cpur"][r] <= 0.85 * P["cpur"][r]]
        print(f"| {c} | {a} | {len(n50)}/4 | {len(ncp)}/4 | " + ", ".join(f"{P['p50r'][r]:.0f}/{f['p50r'][r]:.0f}" for r in sorted(P["p50r"])) + " |")
print("\n## Same-order gap: P vs its own forced order (planned-path overhead)\n")
print("| cell | P p50 us | same-order forced p50 us | gap | noise floor (W2 vs W:first) |\n|---|---|---|---|---|")
for c in cells:
    po = order_of(PL["informed"][c]); pa = [a for (cc, a) in G if cc == c and a.startswith("P:")][0]
    first = [a.split(":")[1] for (cc, a) in G if cc == c and a.startswith("W2:")][0]
    fo = G[(c, "W:" + po)]["p50"]; P = G[(c, pa)]["p50"]
    nf = abs(G[(c, "W2:" + first)]["p50"] - G[(c, "W:" + first)]["p50"]) / G[(c, "W:" + first)]["p50"]
    print(f"| {c} | {P:.0f} | {fo:.0f} | {(P - fo) / fo * 100:.1f} % | {nf * 100:.1f} % |")
print("\n## Per-round p50 us, 200-row cells (arm: rounds 1-4)\n")
for c in cells:
    if c.endswith("-200"):
        print(c, {a: [round(G[(c, a)]["p50r"][r]) for r in sorted(G[(c, a)]["p50r"])] for (cc, a) in G if cc == c}, "\n")
