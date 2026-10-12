import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

OUT = []


def P(s=''):
    OUT.append(s)


def isorow(cell):
    """S1 iso median-of-3 and S2 iso r1"""
    ic = ISO[cell]
    s1 = [load(S1, 'relaxed', ic, 'base', r) for r in (1, 2, 3)]
    s2 = load(S2, 'relaxed', ic, 'base', 1)
    return s1, s2


def medrow(js, key):
    a = [j[key] for j in js]
    return [med([x[p] for x in a]) for p in PCT], a[0]['n']


def table(key, title):
    P(f'**{title}**')
    P()
    P('| cell | arm | runs | n per run | ' + ' | '.join(PCT_NAMES) + ' |')
    P('|---|---|---|---|' + '---|' * len(PCT_NAMES))
    for cell in CELLS:
        s1, s2 = isorow(cell)
        k = key if cell != 'c2' else key.replace('read_', 'writer_')
        for lab, js in ((f'{ISO[cell]} (BQ-S1, base)', s1), (f'{ISO[cell]} (BQ-S2, base)', [s2])):
            v, n = medrow(js, k)
            P(f'| {cell} | {lab} | {len(js)} | {n:,} | ' + ' | '.join(us(x) for x in v) + ' |')
        for arm in ARMS:
            js = [load(S2, 'relaxed', cell, arm, r) for r in (1, 2, 3)]
            v, n = medrow(js, k)
            P(f'| {cell} | {arm} ({SEG[arm]:,} MiB) | 3 | {n:,} | ' + ' | '.join(us(x) for x in v) + ' |')
    P()


table('read_from_schedule', 'Schedule latency, median of the three runs per percentile (isolated arms are controls)')
table('read_service', 'Service latency, median of the three runs per percentile')


# per-run p99 / p99.9, spread, ratios
def runs_vals(cell, arm, key, p):
    return [load(S2, 'relaxed', cell, arm, r)[key][p] for r in (1, 2, 3)]


P('**The bar (§5.3 terms i and ii), per run, schedule latency, relaxed**')
P()
P('Isolated baselines: BQ-S1 (3 runs, same base binary, quiet host) and BQ-S2 (1 run, host settling after the builds). '
  '"Most generous" = the largest of those four isolated values.')
P()
P('| cell | arm | p99 r1 / r2 / r3 | spread of p99 | median p99 ÷ S1 iso median | median p99 ÷ S2 iso | best run ÷ most generous iso | p99.9 r1 / r2 / r3 | spread of p99.9 | median p99.9 ÷ S1 iso median | best run ÷ most generous iso |')
P('|---|---|---|---|---|---|---|---|---|---|---|')
allmiss = []
for cell in CELLS:
    k = 'read_from_schedule' if cell != 'c2' else 'writer_from_schedule'
    s1, s2 = isorow(cell)
    i99 = [j[k]['p99_us'] for j in s1] + [s2[k]['p99_us']]
    i999 = [j[k]['p99.9_us'] for j in s1] + [s2[k]['p99.9_us']]
    s1m99 = med([j[k]['p99_us'] for j in s1])
    s1m999 = med([j[k]['p99.9_us'] for j in s1])
    for arm in ARMS:
        a = runs_vals(cell, arm, k, 'p99_us')
        b = runs_vals(cell, arm, k, 'p99.9_us')
        for ri, (x, y) in enumerate(zip(a, b), 1):
            allmiss.append((cell, arm, x / max(i99), y / max(i999), ri))
        P(f'| {cell} | {arm} | {" / ".join(us(x) for x in a)} | {us(max(a) - min(a))} | {med(a) / s1m99:,.0f}x | '
          f'{med(a) / s2[k]["p99_us"]:,.1f}x | {min(a) / max(i99):,.1f}x | {" / ".join(us(x) for x in b)} | '
          f'{us(max(b) - min(b))} | {med(b) / s1m999:,.0f}x | {min(b) / max(i999):,.1f}x |')
P()
worst = min(allmiss, key=lambda x: x[2])
P(f'Across the {len(allmiss)} runs, the smallest p99 ratio to the most generous isolated value is {worst[2]:.2f}x ({worst[0]}, {worst[1]} r{worst[4]}); {sum(1 for x in allmiss if x[2] <= 2)} runs have a p99 ratio of 2 or less; '
  f'{sum(1 for x in allmiss if x[3] <= 5)} have a p99.9 ratio of 5 or less (all in C2, against the contaminated isolated arm); '
  f'{sum(1 for x in allmiss if x[2] <= 2 and x[3] <= 5)} meet both terms.')
P()
print('\n'.join(OUT))
