#!/usr/bin/env python3
"""Analysis of bn_census --arm A,A2,B lifetimes (one --runs 1 invocation each).
usage: an.py out_c1_*.json   (all files are lifetimes of one configuration)
Per (shape, path) cell and lifetime: block-paired server-CPU deltas (us a statement) of B-A and A2-A,
how many blocks had B above A; unclean cells (host['clean'] False) are dropped from the paired tables."""
import collections, json, random, statistics as st, sys

files = sys.argv[1:]
LT = []
for f in files:
    d = json.load(open(f))
    LT.append(d["runs"][0])
cells = []
for r in LT:
    for a in r["arms"]:
        k = (a["shape"], a["path"])
        if k not in cells:
            cells.append(k)

def get(r, k):
    out = {}
    for a in r["arms"]:
        if (a["shape"], a["path"]) == k:
            out[a["bin"]] = a
    return out

def med(x): return st.median(x)
def boot(x, n=2000, seed=1):
    rnd = random.Random(seed)
    m = sorted(med([rnd.choice(x) for _ in x]) for _ in range(n))
    return m[int(.025 * n)], m[int(.975 * n)]

print("## per cell, per lifetime: median over blocks of the block-paired delta (us a statement)\n")
summary = {}
for k in cells:
    rows, pooledBA, pooledNA = [], [], []
    lifeBA, lifeNA, above, nb, clean_n = [], [], 0, 0, 0
    cpu = collections.defaultdict(list)
    for i, r in enumerate(LT, 1):
        c = get(r, k)
        if len(c) < 3: continue
        clean = c["A"]["host"]["clean"]
        att = c["A"]["host"]["attempts"]
        a, a2, b = (c[x]["block_server_us"] for x in ("A", "A2", "B"))
        dBA = [y - x for x, y in zip(a, b)]
        dNA = [y - x for x, y in zip(a, a2)]
        ab = sum(1 for d in dBA if d > 0)
        rows.append((i, c["A"]["server_cpu_us"], c["A2"]["server_cpu_us"], c["B"]["server_cpu_us"],
                     med(dBA), med(dNA), ab, len(dBA), clean, att))
        if clean:
            pooledBA += dBA; pooledNA += dNA; lifeBA.append(med(dBA)); lifeNA.append(med(dNA))
            above += ab; nb += len(dBA); clean_n += 1
            for x in ("A", "A2", "B"): cpu[x].append(c[x]["server_cpu_us"])
    print(f"### {k[0]} / {k[1]}\n")
    print("| Lifetime | A | A' | B | B - A (median of blocks) | A' - A (floor) | blocks B > A | clean (attempts) |")
    print("|---|---|---|---|---|---|---|---|")
    for (i, ca, ca2, cb, dba, dna, ab, n, clean, att) in rows:
        print(f"| {i} | {ca:.2f} us | {ca2:.2f} us | {cb:.2f} us | {dba:+.2f} us | {dna:+.2f} us | {ab} of {n} | {'yes' if clean else 'NO'} ({att}) |")
    if pooledBA:
        lo, hi = boot(pooledBA)
        llo, lhi = boot(lifeBA, seed=2) if len(lifeBA) > 2 else (min(lifeBA), max(lifeBA))
        print(f"| **pooled, {clean_n} clean lifetimes** | {st.mean(cpu['A']):.2f} us | {st.mean(cpu['A2']):.2f} us | {st.mean(cpu['B']):.2f} us | "
              f"**{med(pooledBA):+.2f} us** [{lo:+.2f}, {hi:+.2f}] blocks; lifetime medians median {med(lifeBA):+.2f} [{llo:+.2f}, {lhi:+.2f}], range {min(lifeBA):+.2f}..{max(lifeBA):+.2f} | "
              f"{med(pooledNA):+.2f} us; lifetime medians {med(lifeNA):+.2f}, range {min(lifeNA):+.2f}..{max(lifeNA):+.2f} | {above} of {nb} | |")
        summary[k] = dict(A=st.mean(cpu['A']), BA=med(pooledBA), BAci=(lo, hi), life=lifeBA, lifeNA=lifeNA, NA=med(pooledNA),
                          above=above, nb=nb, pooledBA=pooledBA, pooledNA=pooledNA)
    print()

print("## headline per cell (pooled over clean lifetimes)\n")
print("| Cell | A server CPU | B - A pooled median [95 % bootstrap over blocks] | lifetime medians of B - A above 0 | A' - A pooled median (floor) | lifetime medians of A' - A: range | blocks B > A | server CPU delta as share of A |")
print("|---|---|---|---|---|---|---|---|")
for k in cells:
    if k not in summary: continue
    m = summary[k]
    print(f"| {k[0]} {k[1]} | {m['A']:.2f} us | {m['BA']:+.2f} us [{m['BAci'][0]:+.2f}, {m['BAci'][1]:+.2f}] | {sum(1 for x in m['life'] if x>0)} of {len(m['life'])} | {m['NA']:+.2f} us | {min(m['lifeNA']):+.2f}..{max(m['lifeNA']):+.2f} us | {m['above']} of {m['nb']} | {100*m['BA']/m['A']:+.2f} % |")
print()
print("## per shape, both paths pooled (blocks of literal and prepared together)\n")
print("| Shape | A server CPU | B - A pooled median [95 % bootstrap over blocks] | A' - A pooled median | lifetime-median B - A: median [95 % bootstrap over lifetimes], range, lifetimes above 0 | lifetime-median A' - A: median [95 % bootstrap over lifetimes], range, lifetimes above 0 | blocks B > A |")
print("|---|---|---|---|---|---|---|")
for shape in dict.fromkeys(k[0] for k in cells):
    ks = [k for k in cells if k[0] == shape and k in summary]
    if not ks: continue
    pba = sum((summary[k]["pooledBA"] for k in ks), [])
    pna = sum((summary[k]["pooledNA"] for k in ks), [])
    nl = min(len(summary[k]["life"]) for k in ks)
    lm = [st.mean(summary[k]["life"][j] for k in ks) for j in range(nl)]
    ln = [st.mean(summary[k]["lifeNA"][j] for k in ks) for j in range(nl)]
    lo, hi = boot(pba)
    ab = sum(summary[k]["above"] for k in ks); nb = sum(summary[k]["nb"] for k in ks)
    print(f"| {shape} | {st.mean(summary[k]['A'] for k in ks):.2f} us | {med(pba):+.2f} us [{lo:+.2f}, {hi:+.2f}] | {med(pna):+.2f} us | {med(lm):+.2f} us [{boot(lm,seed=3)[0]:+.2f}, {boot(lm,seed=3)[1]:+.2f}], {min(lm):+.2f}..{max(lm):+.2f}, {sum(1 for x in lm if x>0)} of {nl} | {med(ln):+.2f} us [{boot(ln,seed=4)[0]:+.2f}, {boot(ln,seed=4)[1]:+.2f}], {min(ln):+.2f}..{max(ln):+.2f}, {sum(1 for x in ln if x>0)} of {nl} | {ab} of {nb} |")

print("\n## latency, driver-side, median over lifetimes of each lifetime's per-arm statistic (the driver reports p90, not p95)\n")
print("| Cell | Arm | statements an arm | QPS (1e6 / mean) | p0 | p25 | p50 | p90 | p99 | server CPU | driver CPU |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for k in cells:
    for arm in ("A", "A2", "B"):
        xs = [get(r, k)[arm] for r in LT if arm in get(r, k)]
        f = lambda key: med([x[key] for x in xs])
        print(f"| {k[0]} {k[1]} | {'A'+chr(39) if arm=='A2' else arm} | {sum(x['ops'] for x in xs):,} | {1e6/f('mean_us'):,.0f} qps | {f('p0_us'):.1f} us | {f('p25_us'):.1f} us | {f('p50_us'):.1f} us | {f('p90_us'):.1f} us | {f('p99_us'):.1f} us | {f('server_cpu_us'):.2f} us | {f('driver_cpu_us'):.2f} us |")
bad = sum(a["bad"] for r in LT for a in r["arms"])
print(f"\nbad replies over every arm, cell and lifetime: {bad}")
hosts = [(r["host_before"], r["host_after"]) for r in LT]
print("idle server CPU (us per second) per lifetime:", [{k: round(v) for k, v in r["idle_cpu_us_per_s"].items()} for r in LT])
unclean = sum(1 for r in LT for a in r["arms"] if a["bin"] == "A" and not a["host"]["clean"])
total = sum(1 for r in LT for a in r["arms"] if a["bin"] == "A")
print(f"unclean cells: {unclean} of {total}; quiet waited s per cell (sum): {sum(a['host']['quiet_waited_s'] for r in LT for a in r['arms'] if a['bin']=='A'):.0f}")
