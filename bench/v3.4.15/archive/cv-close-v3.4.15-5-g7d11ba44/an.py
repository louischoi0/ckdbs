import json, statistics as st, collections
r = json.load(open('/home/louis/ck-tmp/cv-close/q1.json'))
g = collections.defaultdict(list)
for x in r: g[(x['shape'], x['rows'], x['bin'])].append(x)
keys = []
for x in r:
    k = (x['shape'], x['rows'])
    if k not in keys: keys.append(k)
def med(l, f): return st.median(f(x) for x in l)
def rng(l, f):
    v = [f(x) for x in l]; return min(v), max(v)
print("| shape | rows | arm | ops/run | p0 | p25 | p50 | p95* | p99 | mean | QPS (derived) | server CPU/stmt | A2 vs A (cpu) | B vs A (cpu) | B vs A (p50) | verdict |")
for k in keys:
    a = g[k + ('A',)]; b = g[k + ('B',)]; a2 = g[k + ('A2',)]
    cA = med(a, lambda x: x['server_cpu_us'])
    cB = med(b, lambda x: x['server_cpu_us'])
    cA2 = med(a2, lambda x: x['server_cpu_us'])
    pA = med(a, lambda x: x['p50_us']); pB = med(b, lambda x: x['p50_us']); pA2 = med(a2, lambda x: x['p50_us'])
    floor = abs(cA2 - cA) / cA * 100
    dB = (cB - cA) / cA * 100
    # run-to-run spread of A (range / median)
    lo, hi = rng(a, lambda x: x['server_cpu_us']); spread = (hi - lo) / cA * 100
    nz = max(floor, spread)
    v = "slower beyond floor" if dB > nz and dB > 0 else ("faster beyond floor" if -dB > nz else "within floor")
    for lab, l in (('A', a), ('A2', a2), ('B', b)):
        print(f"| {k[0]} | {k[1]} | {lab} | {l[0]['ops']} | {med(l, lambda x: x['p0_us']):.1f} | {med(l, lambda x: x['p25_us']):.1f} | {med(l, lambda x: x['p50_us']):.1f} | {med(l, lambda x: x['p75_us']):.1f} | {med(l, lambda x: x['p99_us']):.1f} | {med(l, lambda x: x['mean_us']):.1f} | {1e6/med(l, lambda x: x['mean_us']):,.0f} | {med(l, lambda x: x['server_cpu_us']):.2f} | | | | |")
    print(f"| SUM {k[0]} | {k[1]} | | | | | | | | | | | {(cA2-cA)/cA*100:+.1f} % (A spread {spread:.1f} %) | {dB:+.1f} % ({cB-cA:+.2f} us) | {(pB-pA)/pA*100:+.1f} % | {v} |")
