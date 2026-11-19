#!/usr/bin/env python3
"""Knob analysis: bn_census lifetimes with arms A, A2 (A's binary again), B, K1, K2, K3, KA.
usage: an7.py out_k_*.json"""
import json, random, statistics as st, sys

LT = [json.load(open(f))["runs"][0] for f in sys.argv[1:]]
ARMS = ["A", "A2", "B", "K1", "K2", "K3", "KA"]
PAIRS = [("B", "A", "B - A"), ("K1", "A", "K1 - A"), ("K2", "A", "K2 - A"), ("K3", "A", "K3 - A"), ("KA", "A", "KA - A"),
         ("B", "K1", "B - K1"), ("B", "K2", "B - K2"), ("B", "K3", "B - K3"), ("B", "KA", "B - KA"), ("A2", "A", "A' - A")]
NAMES = [p[2] for p in PAIRS]
cells = []
for r in LT:
    for a in r["arms"]:
        k = (a["shape"], a["path"])
        if k not in cells:
            cells.append(k)


def get(r, k): return {a["bin"]: a for a in r["arms"] if (a["shape"], a["path"]) == k}
def med(x): return st.median(x)


def boot(x, n=2000, seed=1):
    rnd = random.Random(seed)
    m = sorted(med([rnd.choice(x) for _ in x]) for _ in range(n))
    return m[int(.025 * n)], m[int(.975 * n)]


S = {}
print("## per cell, per lifetime (median over blocks of the block-paired delta, us a statement)\n")
for k in cells:
    P = {n: {"blocks": [], "life": []} for n in NAMES}
    cpu = {x: [] for x in ARMS}
    rows = []
    for i, r in enumerate(LT, 1):
        c = get(r, k)
        if len(c) < len(ARMS): continue
        clean = c["A"]["host"]["clean"]
        bl = {x: c[x]["block_server_us"] for x in c}
        ds = {n: [p - q for q, p in zip(bl[y], bl[x])] for (x, y, n) in PAIRS}
        rows.append((i, [c[x]["server_cpu_us"] for x in ARMS], [med(ds[n]) for n in NAMES], clean))
        if clean:
            for n in NAMES:
                P[n]["blocks"] += ds[n]; P[n]["life"].append(med(ds[n]))
            for x in ARMS: cpu[x].append(c[x]["server_cpu_us"])
    print(f"### {k[0]} / {k[1]}\n")
    print("| Lifetime | " + " | ".join(ARMS[:2] + ARMS[2:]) .replace("A2", "A'") + " | " + " | ".join(NAMES) + " | clean |")
    print("|" + "---|" * (len(ARMS) + len(NAMES) + 2))
    for (i, cp, ds, clean) in rows:
        print(f"| {i} | " + " | ".join(f"{x:.2f} us" for x in cp) + " | " + " | ".join(f"{d:+.2f} us" for d in ds) + f" | {'yes' if clean else 'NO'} |")
    S[k] = {"P": P, "cpu": {x: st.mean(v) for x, v in cpu.items() if v}}
    print()

print("## pooled per cell: median of block-paired deltas [95 % bootstrap over blocks], lifetime medians above 0, blocks above 0\n")
for k in cells:
    P = S[k]["P"]
    print(f"### {k[0]} / {k[1]} (A server CPU {S[k]['cpu']['A']:.2f} us; arms' mean server CPU: " +
          ", ".join(f"{x} {S[k]['cpu'][x]:.2f}" for x in ARMS) + ")\n")
    print("| Pair | Pooled median [95 % bootstrap] | Lifetime medians: median, range, above 0 | Blocks above 0 |")
    print("|---|---|---|---|")
    for n in NAMES:
        b = P[n]["blocks"]; lo, hi = boot(b); L = P[n]["life"]
        print(f"| {n} | {med(b):+.2f} us [{lo:+.2f}, {hi:+.2f}] | {med(L):+.2f} us, {min(L):+.2f}..{max(L):+.2f}, {sum(1 for x in L if x > 0)} of {len(L)} | {sum(1 for x in b if x > 0)} of {len(b)} |")
    print()

print("## headline matrix: pooled median [95 % bootstrap over blocks] by cell\n")
print("| Cell | A | " + " | ".join(NAMES) + " |")
print("|---|---|" + "---|" * len(NAMES))
for k in cells:
    P = S[k]["P"]
    row = []
    for n in NAMES:
        b = P[n]["blocks"]; lo, hi = boot(b)
        row.append(f"{med(b):+.2f} [{lo:+.2f}, {hi:+.2f}]")
    print(f"| {k[0]} {k[1]} | {S[k]['cpu']['A']:.2f} us | " + " | ".join(row) + " |")
print("\n(all deltas in us of server CPU a statement)\n")

print("## per shape, both paths pooled\n")
print("| Shape | " + " | ".join(NAMES) + " |")
print("|---|" + "---|" * len(NAMES))
for shape in dict.fromkeys(k[0] for k in cells):
    ks = [k for k in cells if k[0] == shape]
    row = []
    for n in NAMES:
        b = sum((S[k]["P"][n]["blocks"] for k in ks), []); lo, hi = boot(b)
        row.append(f"{med(b):+.2f} [{lo:+.2f}, {hi:+.2f}]")
    print(f"| {shape} | " + " | ".join(row) + " |")
print("\n(us; lifetime medians per shape of the same pairs)\n")
print("| Shape | " + " | ".join(NAMES) + " |")
print("|---|" + "---|" * len(NAMES))
for shape in dict.fromkeys(k[0] for k in cells):
    ks = [k for k in cells if k[0] == shape]
    row = []
    for n in NAMES:
        nl = min(len(S[k]["P"][n]["life"]) for k in ks)
        lm = [st.mean(S[k]["P"][n]["life"][j] for k in ks) for j in range(nl)]
        lo, hi = boot(lm, seed=3)
        row.append(f"{med(lm):+.2f} [{lo:+.2f}, {hi:+.2f}], {sum(1 for x in lm if x > 0)} of {nl}")
    print(f"| {shape} | " + " | ".join(row) + " |")

print("\n## latency, driver-side (median over lifetimes of each lifetime's per-arm statistic)\n")
print("| Cell | Arm | statements | QPS (1e6 / mean) | p0 | p25 | p50 | p90 | p99 | server CPU | driver CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for k in cells:
    for arm in ARMS:
        xs = [get(r, k)[arm] for r in LT if arm in get(r, k)]
        g = lambda key: med([x[key] for x in xs])
        nm = "A'" if arm == "A2" else arm
        print(f"| {k[0]} {k[1]} | {nm} | {sum(x['ops'] for x in xs):,} | {1e6/g('mean_us'):,.0f} qps | {g('p0_us'):.1f} us | {g('p25_us'):.1f} us | {g('p50_us'):.1f} us | {g('p90_us'):.1f} us | {g('p99_us'):.1f} us | {g('server_cpu_us'):.2f} us | {g('driver_cpu_us'):.2f} us |")
print(f"\nbad replies: {sum(a['bad'] for r in LT for a in r['arms'])}")
un = sum(1 for r in LT for a in r["arms"] if a["bin"] == "A" and not a["host"]["clean"])
tot = sum(1 for r in LT for a in r["arms"] if a["bin"] == "A")
print(f"unclean cells: {un} of {tot}; cell-gate seconds waited: {sum(a['host']['quiet_waited_s'] for r in LT for a in r['arms'] if a['bin']=='A'):.0f}")
loads = [float(a['host']['loadavg'].split()[0]) for r in LT for a in r['arms'] if 'loadavg' in a['host']]
print(f"1-minute load at the cells: {min(loads):.2f}..{max(loads):.2f}")
