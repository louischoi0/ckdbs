#!/usr/bin/env python3
"""Analysis of bn_census --arm A,A2,B,C lifetimes (one --runs 1 invocation each).
usage: an4.py out_r4_*.json
Block-paired server-CPU deltas (us a statement) of B-A, C-A, C-B and A2-A (the floor); unclean cells dropped."""
import json, random, statistics as st, sys

LT = [json.load(open(f))["runs"][0] for f in sys.argv[1:]]
cells = []
for r in LT:
    for a in r["arms"]:
        k = (a["shape"], a["path"])
        if k not in cells:
            cells.append(k)
PAIRS = [("B", "A", "B - A"), ("C", "A", "C - A"), ("C", "B", "C - B"), ("A2", "A", "A' - A")]


def get(r, k):
    return {a["bin"]: a for a in r["arms"] if (a["shape"], a["path"]) == k}


def med(x): return st.median(x)


def boot(x, n=2000, seed=1):
    rnd = random.Random(seed)
    m = sorted(med([rnd.choice(x) for _ in x]) for _ in range(n))
    return m[int(.025 * n)], m[int(.975 * n)]


def f(v): return f"{v:+.2f} us"


S = {}  # cell -> pair -> dict
print("## per cell, per lifetime (median over blocks of the block-paired delta, us a statement)\n")
for k in cells:
    rows = []
    P = {p[2]: {"blocks": [], "life": []} for p in PAIRS}
    cpu = {x: [] for x in ("A", "A2", "B", "C")}
    cltB = nb = 0
    for i, r in enumerate(LT, 1):
        c = get(r, k)
        if len(c) < 4:
            continue
        clean = c["A"]["host"]["clean"]
        bl = {x: c[x]["block_server_us"] for x in c}
        ds = {}
        for (x, y, name) in PAIRS:
            ds[name] = [p - q for q, p in zip(bl[y], bl[x])]
        cb = sum(1 for d in ds["C - B"] if d < 0)
        rows.append((i, [c[x]["server_cpu_us"] for x in ("A", "A2", "B", "C")], [med(ds[n[2]]) for n in PAIRS], cb, len(ds["C - B"]), clean, c["A"]["host"]["attempts"]))
        if clean:
            for (x, y, name) in PAIRS:
                P[name]["blocks"] += ds[name]
                P[name]["life"].append(med(ds[name]))
            for x in cpu: cpu[x].append(c[x]["server_cpu_us"])
            cltB += cb; nb += len(ds["C - B"])
    print(f"### {k[0]} / {k[1]}\n")
    print("| Lifetime | A | A' | B | C | B - A | C - A | C - B | A' - A (floor) | blocks C < B | clean (attempts) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for (i, cp, ds, cb, n, clean, att) in rows:
        print(f"| {i} | " + " | ".join(f"{x:.2f} us" for x in cp) + " | " + " | ".join(f(d) for d in ds) +
              f" | {cb} of {n} | {'yes' if clean else 'NO'} ({att}) |")
    if P["B - A"]["blocks"]:
        cells_txt = []
        for (_, _, name) in PAIRS:
            b = P[name]["blocks"]
            lo, hi = boot(b)
            cells_txt.append(f"**{med(b):+.2f} us** [{lo:+.2f}, {hi:+.2f}]")
        print("| **pooled** | " + " | ".join(f"{st.mean(cpu[x]):.2f} us" for x in ("A", "A2", "B", "C")) + " | " +
              " | ".join(cells_txt) + f" | {cltB} of {nb} | {len(P['B - A']['life'])} clean |")
    S[k] = {"P": P, "cpu": {x: st.mean(v) for x, v in cpu.items() if v}, "cltB": cltB, "nb": nb}
    print()

print("## headline per cell (pooled over clean lifetimes; median of block-paired deltas [95 % bootstrap over blocks]; lifetimes above 0)\n")
print("| Cell | A server CPU | B - A | C - A | C - B | A' - A (floor) | lifetime medians of A' - A: range | blocks C < B | C - B as share of A |")
print("|---|---|---|---|---|---|---|---|---|")
for k in cells:
    if k not in S: continue
    P = S[k]["P"]
    t = []
    for name in ("B - A", "C - A", "C - B", "A' - A"):
        b = P[name]["blocks"]; lo, hi = boot(b)
        up = sum(1 for x in P[name]["life"] if x > 0)
        t.append(f"{med(b):+.2f} us [{lo:+.2f}, {hi:+.2f}], {up} of {len(P[name]['life'])}")
    fl = P["A' - A"]["life"]
    print(f"| {k[0]} {k[1]} | {S[k]['cpu']['A']:.2f} us | " + " | ".join(t) +
          f" | {min(fl):+.2f}..{max(fl):+.2f} us | {S[k]['cltB']} of {S[k]['nb']} | {100*med(P['C - B']['blocks'])/S[k]['cpu']['A']:+.2f} % |")

print("\n## per shape, both paths pooled\n")
print("| Shape | A server CPU | B - A | C - A | C - B | A' - A (floor) | lifetime-median C - B: median [95 % bootstrap over lifetimes], range, lifetimes above 0 | blocks C < B |")
print("|---|---|---|---|---|---|---|---|")
for shape in dict.fromkeys(k[0] for k in cells):
    ks = [k for k in cells if k[0] == shape and k in S]
    if not ks: continue
    t = []
    for name in ("B - A", "C - A", "C - B", "A' - A"):
        b = sum((S[k]["P"][name]["blocks"] for k in ks), [])
        lo, hi = boot(b)
        t.append(f"{med(b):+.2f} us [{lo:+.2f}, {hi:+.2f}]")
    nl = min(len(S[k]["P"]["C - B"]["life"]) for k in ks)
    lm = [st.mean(S[k]["P"]["C - B"]["life"][j] for k in ks) for j in range(nl)]
    lo, hi = boot(lm, seed=3)
    print(f"| {shape} | {st.mean(S[k]['cpu']['A'] for k in ks):.2f} us | " + " | ".join(t) +
          f" | {med(lm):+.2f} us [{lo:+.2f}, {hi:+.2f}], {min(lm):+.2f}..{max(lm):+.2f}, {sum(1 for x in lm if x > 0)} of {nl} | "
          f"{sum(S[k]['cltB'] for k in ks)} of {sum(S[k]['nb'] for k in ks)} |")

print("\n## latency, driver-side (median over lifetimes of each lifetime's per-arm statistic)\n")
print("| Cell | Arm | statements | QPS (1e6 / mean) | p0 | p25 | p50 | p90 | p99 | server CPU | driver CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for k in cells:
    for arm in ("A", "A2", "B", "C"):
        xs = [get(r, k)[arm] for r in LT if arm in get(r, k)]
        g = lambda key: med([x[key] for x in xs])
        nm = "A'" if arm == "A2" else arm
        print(f"| {k[0]} {k[1]} | {nm} | {sum(x['ops'] for x in xs):,} | {1e6/g('mean_us'):,.0f} qps | {g('p0_us'):.1f} us | {g('p25_us'):.1f} us | {g('p50_us'):.1f} us | {g('p90_us'):.1f} us | {g('p99_us'):.1f} us | {g('server_cpu_us'):.2f} us | {g('driver_cpu_us'):.2f} us |")
print(f"\nbad replies: {sum(a['bad'] for r in LT for a in r['arms'])}")
un = sum(1 for r in LT for a in r["arms"] if a["bin"] == "A" and not a["host"]["clean"])
tot = sum(1 for r in LT for a in r["arms"] if a["bin"] == "A")
print(f"unclean cells: {un} of {tot}; cell-gate seconds waited (sum over cells): {sum(a['host']['quiet_waited_s'] for r in LT for a in r['arms'] if a['bin']=='A'):.0f}")
loads = [float(a['host']['loadavg'].split()[0]) for r in LT for a in r['arms'] if 'loadavg' in a['host']]
print(f"1-minute load at the cells: {min(loads):.2f}..{max(loads):.2f}")
