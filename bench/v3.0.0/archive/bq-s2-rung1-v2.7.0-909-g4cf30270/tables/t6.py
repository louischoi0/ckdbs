import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *
import json
nb = json.load(open('/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad/nb.json'))
rows = []
for cell in CELLS:
    k = 'read_from_schedule' if cell != 'c2' else 'writer_from_schedule'
    ic = ISO[cell]
    iso = [load(S1, 'relaxed', ic, None, r)[k] for r in (1, 2, 3)] + [load(S2, 'relaxed', ic, 'base', 1)[k]]
    g99 = max(i['p99_us'] for i in iso); g999 = max(i['p99.9_us'] for i in iso)
    s1m99 = med([i['p99_us'] for i in iso[:3]]); s1m999 = med([i['p99.9_us'] for i in iso[:3]])
    for arm in ARMS:
        for r in (1, 2, 3):
            a = load(S2, 'relaxed', cell, arm, r)[k]
            rows.append((a['p99_us'] / g99, a['p99.9_us'] / g999, a['p99_us'] / s1m99, a['p99.9_us'] / s1m999, cell, arm, r))
print('lowest by p99 (ratio to most generous | p99.9 ratio | p99 vs S1 median | p99.9 vs S1 median)')
for x in sorted(rows)[:10]:
    print([round(v, 1) if isinstance(v, float) else v for v in x])
print('lowest by p99 vs S1 median')
for x in sorted(rows, key=lambda t: t[2])[:8]:
    print([round(v, 1) if isinstance(v, float) else v for v in x])
print('C1 only lowest p99 vs S1 median')
for x in sorted([x for x in rows if x[4] != 'c2'], key=lambda t: t[2])[:8]:
    print([round(v, 1) if isinstance(v, float) else v for v in x])
print('non-bulk values: lowest 8 p99')
l = sorted([(v[0], k) for k, v in nb.items() if isinstance(v[0], (int, float)) and v[0] > 0])[:8]
print(l)
print('NB < 1ms count', sum(1 for v in nb.values() if v[0] == 0.0), sum(1 for v in nb.values() if v[0] is None))
# how many C1/C2 runs have p99 > 100 ms
print('runs with agg p99 > 100 ms', sum(1 for x in rows if x[2] * (med([load(S1,'relaxed',ISO[x[4]],None,r)['read_from_schedule' if x[4]!='c2' else 'writer_from_schedule']['p99_us'] for r in (1,2,3)])) > 100000))
