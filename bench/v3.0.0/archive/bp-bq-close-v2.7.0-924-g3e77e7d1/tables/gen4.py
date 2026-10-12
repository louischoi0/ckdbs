import sys, os, statistics
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lib import *

# values typed from bench/v3.0.0/results-bp-s3-g1-ab-v2.7.0-909-gccf9cfc1.md (section 2 table), r1 / r2 / r3
S3 = {
    ('select-pk', 'literal'): ([26.08, 25.18, 26.98], [25.20, 26.16, 26.83]),
    ('select-pk', 'prepared'): ([25.78, 24.58, 27.05], [25.22, 25.43, 25.74]),
    ('update-lit', 'literal'): ([24.93, 23.46, 23.97], [24.67, 24.13, 24.66]),
    ('update-lit', 'prepared'): ([24.22, 22.49, 23.51], [23.46, 23.34, 24.05]),
    ('update-range', 'literal'): ([7055.4, 6861.4, 6977.0], [6865.6, 6771.0, 6963.3]),
    ('update-range', 'prepared'): ([7203.6, 6994.7, 7074.5], [6974.7, 6894.6, 6943.8]),
}
LAB = {('select-pk', 'literal'): 'point `SELECT`, literal', ('select-pk', 'prepared'): 'point `SELECT`, prepared', ('update-lit', 'literal'): 'point `UPDATE`, literal',
       ('update-lit', 'prepared'): 'point `UPDATE`, prepared', ('update-range', 'literal'): 'every-row `UPDATE`, literal', ('update-range', 'prepared'): 'every-row `UPDATE`, prepared'}
# today's 1592007e: second server of S2S1; ccf9cfc1: first server of S4S6BQ, MERGE, PREMERGE_FIX
t1592 = {}
tccf = {}
for pair, store, side in (('S2S1', t1592, 'B'), ('S4S6BQ', tccf, 'A'), ('MERGE', tccf, 'A'), ('PREMERGE_FIX', tccf, 'A')):
    out, runs = load_pair(pair)
    for key, arms in out.items():
        store.setdefault(key, []).extend(x['server_cpu_us'] for x in arms[side])
lines = ['| shape (driver path) | `1592007e` in BP-S3\'s file (mean of 3) | `1592007e` here (mean of 3, S2S1) | here vs there | `ccf9cfc1` in BP-S3\'s file (mean of 3) | `ccf9cfc1` here (mean of 9) | here vs there |', '|---|---|---|---|---|---|---|']
for key, (a, b) in S3.items():
    if key[0] == 'update-lit':
        continue  # the bisect pairs ran select-pk and update-range only
    nd = 1 if key[0] == 'update-range' else 2
    ma, mb = statistics.mean(a), statistics.mean(b)
    na, nb = statistics.mean(t1592[key]), statistics.mean(tccf[key])
    lines.append(f'| {LAB[key]} | {ma:,.{nd}f} µs | {na:,.{nd}f} µs | {100*(na/ma-1):+.1f} % | {mb:,.{nd}f} µs | {nb:,.{nd}f} µs | {100*(nb/mb-1):+.1f} % |')
open(HERE + '/frag_s3check.md', 'w').write('\n'.join(lines) + '\n')
print('\n'.join(lines))

# numbers for prose
import json
out, runs = load_pair('S4S6BQ')
print('--- premium model')
D = OUT + '/lock'
rows = {}
for mode in ('plain', 'fence'):
    for side in ('S6A', 'B'):
        for r in range(3):
            m = json.load(open(f'{D}/{side}-locks-relaxed-k8-{mode}-r{r}.json'))
            rows[(mode, side, r)] = m['server_cpu_s'] * 1e6 / 96000
pS = [rows[('fence', 'S6A', r)] - rows[('plain', 'S6A', r)] for r in range(3)]
pB = [rows[('fence', 'B', r)] - rows[('plain', 'B', r)] for r in range(3)]
print('premium us S6A', [round(x, 2) for x in pS], 'B', [round(x, 2) for x in pB])
walks = 0.659
nS, nB = walks * 512, walks * 64
b = (statistics.median(pS) - statistics.median(pB)) / (nS - nB)
a = statistics.median(pB) - b * nB
print('latch acquisitions per stmt S6A %.0f B %.0f ; slope %.1f ns per acquisition; fixed %.2f us' % (nS, nB, b * 1000, a))
bs = [(x - y) / (nS - nB) * 1000 for x in pS for y in pB]
print('slope range over pairings', round(min(bs), 1), round(max(bs), 1))
fS = statistics.median(rows[('fence', 'S6A', r)] for r in range(3))
fB = statistics.median(rows[('fence', 'B', r)] for r in range(3))
print('fence median S6A %.3f B %.3f delta %.3f (%.2f%%)' % (fS, fB, fB - fS, 100 * (fB / fS - 1)))
pl = statistics.median(rows[('plain', 'S6A', r)] for r in range(3)), statistics.median(rows[('plain', 'B', r)] for r in range(3))
print('plain median', [round(x, 3) for x in pl], '%.2f%%' % (100 * (pl[1] / pl[0] - 1)))
print('premium of medians S6A %.2f%% B %.2f%%' % (100 * (fS / pl[0] - 1), 100 * (fB / pl[1] - 1)))
print('S6A fence runs', [round(rows[('fence', 'S6A', r)], 2) for r in range(3)], 'B fence runs', [round(rows[('fence', 'B', r)], 2) for r in range(3)])
# per-row costs
for k, (lo, hi) in {'merge pooled lit': (6753.26, 7032.36), 'close pooled lit': (6753.26, 7099.58), 'close pooled prep': (6777.67, 7201.37), 'merge pooled prep': (6777.67, 7111.40)}.items():
    print(k, 'delta us %.0f  ns/row %.1f  %.1f%%' % (hi - lo, (hi - lo) / 10, 100 * (hi / lo - 1)))
