import json, statistics as st
d = "/tmp/claude-1000/-home-cdkbs-ckdbs/00ec4c4c-8445-418e-92d3-c235d2b282e7/scratchpad/cbclose/"
o = json.load(open(d + "out3.json"))
M = st.median
def med(c, e, f):
    return M(x[f] for x in o if x['cell'] == c and x['engine'] == e)
print("| cell | A qps | A2 qps | B qps | C qps | B vs A (qps) | A2 vs A (floor) | B vs A p50 | B vs A srv cpu | C vs A p50 |")
for c in dict.fromkeys(x['cell'] for x in o):
    a, a2, b, bw = (med(c, e, 'qps') for e in ("A","A2","B","C"))
    p = lambda e: med(c, e, 'p50')
    cp = lambda e: med(c, e, 'server_cpu_us')
    print(f"| {c} | {a:,.0f} | {a2:,.0f} | {b:,.0f} | {bw:,.0f} | {100*(b/a-1):+.1f} % | {100*(a2/a-1):+.1f} % | {p('B')-p('A'):+.1f} µs ({100*(p('B')/p('A')-1):+.1f} %) | {cp('B')-cp('A'):+.1f} µs ({100*(cp('B')/cp('A')-1):+.1f} %) | {p('C')-p('A'):+.1f} µs ({100*(p('C')/p('A')-1):+.1f} %) |")
print()
print("| cell | eng | mean wall | server CPU/op | remainder (client + loopback + sched) |")
for c in dict.fromkeys(x['cell'] for x in o):
    for e in ('A', 'B'):
        m, cp = med(c, e, 'mean_us'), med(c, e, 'server_cpu_us')
        print(f"| {c} | {e} | {m:,.1f} µs | {cp:,.1f} µs | {m-cp:,.1f} µs ({100*(m-cp)/m:.0f} %) |")
