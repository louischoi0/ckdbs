#!/usr/bin/env python3
"""BT-S1: prints the markdown tables of results-bt-s1-premise-v2.7.0-992-geeb88f47.md
from bmscan-bn.json.gz (net rows, per-run grids, pooled latency percentiles, the
scan statement's decomposition, host per run).  Usage: bt_s1_fragments.py [json[.gz]]"""
import collections, json, statistics as st, gzip, sys

med = st.median
SRC = sys.argv[1] if len(sys.argv) > 1 else 'bmscan-bn.json.gz'
d = json.load(gzip.open(SRC, 'rt') if SRC.endswith('.gz') else open(SRC))
runs = d['runs']
ARMS = ['A', 'A2', 'D1', 'D2', 'D3', 'all', 'D4']
LBL = {'A': 'A', 'A2': "A'", 'D1': 'D1', 'D2': 'D2', 'D3': 'D3', 'all': 'D-all', 'D4': 'D4'}
PATHS = ('literal', 'prepared')


def cells(kind, p):
    out = collections.defaultdict(list)
    for r in runs:
        for x in r['arms']:
            if x['kind'] == kind and x['path'] == p:
                out[x['arm']].append(x)
    return out


def net(p):
    sc, pt = cells('scan', p), cells('point', p)
    return {a: [(sc[a][i]['server_cpu_us'] - pt[a][i]['server_cpu_us']) * 1e3 / sc[a][i]['rows']
                for i in range(len(runs))] for a in ARMS}


def pct(xs, q):
    xs = sorted(xs)
    k = (len(xs) - 1) * q / 100.0
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def sg(v, nd=1):
    s = ('%+.' + str(nd) + 'f') % v
    return s.replace('-', '−') if False else s


out = {}
# net tables
for p in PATHS:
    n = net(p)
    L = ['| arm | net row, median [min, max] | rows/s (derived, 1e9 / median ns) | arm - A, paired by run, median [min, max] | runs below A |',
         '|---|---|---|---|---|']
    for a in ARMS:
        xs = n[a]
        dd = [b - c for c, b in zip(n['A'], xs)]
        if a == 'A':
            dcell = '(reference)'
            below = '-'
        else:
            dcell = '%+.1f ns [%+.1f, %+.1f]' % (med(dd), min(dd), max(dd))
            below = '%d of 5' % sum(1 for v in dd if v < 0)
        L.append('| %s | %.1f ns [%.1f, %.1f] | %s /s | %s | %s |' % (
            LBL[a], med(xs), min(xs), max(xs), format(int(1e9 / med(xs)), ','), dcell, below))
    out['net_' + p] = '\n'.join(L)
    L = ['| run | ' + ' | '.join(LBL[a] for a in ARMS) + ' |', '|---' * (len(ARMS) + 1) + '|']
    for i in range(len(runs)):
        L.append('| %d | ' % i + ' | '.join('%.1f ns' % n[a][i] for a in ARMS) + ' |')
    out['perrun_' + p] = '\n'.join(L)

# latency
L = ['| path | arm | n | p0 | p25 | p50 | p90 | p95 | p99 | scans/s at p50 (derived) |', '|---|---|---|---|---|---|---|---|---|---|']
for p in PATHS:
    sc = cells('scan', p)
    for a in ARMS:
        lat = [v for c in sc[a] for v in c['lat_us']]
        L.append('| %s | %s | %d | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %s /s |' % (
            p, LBL[a], len(lat), min(lat), pct(lat, 25), pct(lat, 50), pct(lat, 90), pct(lat, 95), pct(lat, 99),
            format(int(1e6 / pct(lat, 50)), ',')))
out['lat'] = '\n'.join(L)

# decomposition
L = ['| path | arm | wall mean | per-row work (10,000 x net row) | fixed statement (point `SELECT` CPU) | client + socket (wall - server CPU) | per-row share of wall |',
     '|---|---|---|---|---|---|---|']
for p in PATHS:
    sc, pt = cells('scan', p), cells('point', p)
    n = net(p)
    for a in ARMS:
        wall = med([c['p_mean'] for c in sc[a]])
        scpu = med([c['server_cpu_us'] for c in sc[a]])
        pcpu = med([c['server_cpu_us'] for c in pt[a]])
        row = med(n[a]) * 10
        L.append('| %s | %s | %.1f µs | %.1f µs | %.1f µs | %.1f µs | %.0f %% |' % (
            p, LBL[a], wall, row, pcpu, wall - scpu, 100 * row / wall))
out['decomp'] = '\n'.join(L)

# host per run
L = ['| run | gate wait (load at release) | cells | load at cells | sibling busy, max | foreign server / driver CPU, max | competing build or test |', '|---|---|---|---|---|---|---|']
for r in runs:
    hs = [x['host'] for x in r['arms']]
    lo, hi = min(float(h['loadavg'].split()[0]) for h in hs), max(float(h['loadavg'].split()[0]) for h in hs)
    L.append('| %d | %.0f s (%.2f) | %d, all clean, 1 attempt | %s | %.1f %% | %.1f %% / %.1f %% | none |' % (
        r['run'], r['quiet_gate']['waited_s'], r['quiet_gate']['load'], len(hs),
        ('%.2f' % lo) if lo == hi else ('%.2f-%.2f' % (lo, hi)),
        max(h['sibling_busy_pct'] for h in hs), max(h['foreign_server_cpu_pct'] for h in hs),
        max(h['foreign_driver_cpu_pct'] for h in hs)))
out['host'] = '\n'.join(L)

for k, v in out.items():
    print('=== ' + k)
    print(v)
