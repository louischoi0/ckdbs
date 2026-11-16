import json, sys, statistics as st
from collections import defaultdict
r = json.load(open(sys.argv[1]))['cells']
d = defaultdict(lambda: defaultdict(list))
for x in r:
    d[(x['cores'], x['dur'], x['conns'], x['k'])][x['arm']].append(x)
def m(xs, k): return st.mean(x[k] for x in xs)
def lp(xs, k): return st.mean(x['lat'][k] for x in xs)
keys = sorted(d, key=lambda k: (k[0], ["strict", "relaxed"].index(k[1]), k[2], k[3]))
print("| cores | dur | conns | k | R groups/s | W groups/s | A groups/s | A2 groups/s | (R-A)/A | (W-A)/A | spread (A,A2) | R reading | R read_only_begins per group | W read_only_begins |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for k in keys:
    a = d[k]
    g = {n: m(a[n], 'gps') for n in a}
    ga = [x['gps'] for x in a['A'] + a['A2']]
    abase = st.mean(ga)
    spread = (max(ga) - min(ga)) / abase
    ra = (g['R'] - abase) / abase
    wa = (g['W'] - abase) / abase
    reading = "R tracks A (inside spread)" if abs(ra) <= spread else ("R above A" if ra > 0 else "R below A")
    robg = sum(x['read_only_begins'] for x in a['R']) / sum(x['groups'] for x in a['R'])
    wob = sum(x['read_only_begins'] for x in a['W'])
    print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {g['R']:.0f} | {g['W']:.0f} | {g['A']:.0f} | {g['A2']:.0f} | {ra*100:+.0f}% | {wa*100:+.0f}% | {spread*100:.0f}% | {reading} | {robg:.3f} | {wob} |")
print()
print("| cores | dur | conns | k | arm | p0 | p25 | p50 | p75 | p99 (us) | groups (4 reps) | WAL B/group | syncs/group |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for k in keys:
    a = d[k]
    for n in ("R", "W", "A"):
        xs = a[n] if n != "A" else a['A'] + a['A2']
        gr = sum(x['groups'] for x in xs)
        wal = sum(x['wal_bytes_per_group'] * x['groups'] for x in xs) / gr
        sy = sum(x['syncs'] for x in xs) / gr
        lab = n + (' (A+A2)' if n == 'A' else '')
        print(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {lab} | {lp(xs,'p0_us'):.0f} | {lp(xs,'p25_us'):.0f} | {lp(xs,'p50_us'):.0f} | {lp(xs,'p75_us'):.0f} | {lp(xs,'p99_us'):.0f} | {gr} | {wal:.2f} | {sy:.4f} |")
print("\nerrors:", len([x for x in r if x['err']]))
loads = [float(x['host']['loadavg'].split()[0]) for x in r]
print("load1 min/med/max", min(loads), st.median(loads), max(loads), "cc1plus max", max(int(x['host']['cc1plus'] or 0) for x in r))
