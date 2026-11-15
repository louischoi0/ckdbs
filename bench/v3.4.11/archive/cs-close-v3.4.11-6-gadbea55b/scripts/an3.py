import json, sys, statistics as st
r = json.load(open(sys.argv[1]))
print("| k | arm | runs | groups/run | server CPU us/group (mean) | min..max | client mean us | p0 | p25 | p50 | p75 | p99 | bad |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for k in (1, 16):
    base = {}
    for arm in ("A", "W", "R"):
        xs = [x for x in r if x['k'] == k and x['arm'] == arm]
        c = [x['server_cpu_us'] for x in xs]
        base[arm] = c
        f = lambda key: st.mean(x[key] for x in xs)
        print(f"| {k} | {arm} | {len(xs)} | {xs[0]['groups']} | {st.mean(c):.2f} | {min(c):.1f}..{max(c):.1f} | {f('mean_us'):.1f} | {f('p0_us'):.0f} | {f('p25_us'):.0f} | {f('p50_us'):.0f} | {f('p75_us'):.0f} | {f('p99_us'):.0f} | {sum(x['bad'] for x in xs)} |")
    for a, b in (("W", "A"), ("R", "A"), ("R", "W")):
        d = [x - y for x, y in zip(base[a], base[b])]
        print(f"|  | {a}-{b} paired server CPU | | | {st.mean(d):+.2f} us | sd {st.stdev(d):.2f} | | | | | | | |")
