import json, statistics as st
d = "/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cbclose/"
o = json.load(open(d + "out3.json"))
M = st.median
print("| cell | eng | rounds | ops/round | p0 | p25 | p50 | p95 | p99 | qps | srvcpu/op | p50 min-max over rounds |")
for c in dict.fromkeys(x['cell'] for x in o):
    for e in ('A', 'A2', 'B', 'C'):
        r = [x for x in o if x['cell'] == c and x['engine'] == e]
        k = lambda f: M(x[f] for x in r)
        ps = [x['p50'] for x in r]
        print(f"| {c} | {e} | {len(r)} | {r[0]['ops']} | {k('p0'):.1f} | {k('p25'):.1f} | {k('p50'):.1f} | {k('p95'):.1f} | {k('p99'):.1f} | {k('qps'):.0f} | {k('server_cpu_us'):.1f} | {min(ps):.1f}-{max(ps):.1f} |")
bad = [x for x in o if x['host']['competing']]
print("cells with competing build/ctest:", len(bad))
print(sorted(set(x['host']['loadavg'].split()[0] for x in o)))
