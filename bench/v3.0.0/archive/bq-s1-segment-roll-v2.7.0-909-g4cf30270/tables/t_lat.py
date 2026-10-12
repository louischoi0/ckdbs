import sys, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *
KEYS = ['p0_us', 'p25_us', 'p50_us', 'p95_us', 'p99_us', 'p99.9_us', 'max_us']


def pick(d, cell, kind):
    if cell.startswith('c2'):
        return d['writer_from_schedule' if kind == 'sched' else 'writer_service']
    return d['read_from_schedule' if kind == 'sched' else 'read_service']


mode = sys.argv[1]
for kind in ('sched', 'serv'):
    for dur in DURS:
        print(f'\n### {mode} {kind} {dur}')
        print('| cell | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |')
        print('|---|---|---|---|---|---|---|---|---|---|')
        for cell in CELLS:
            if cell == 'c3':
                continue
            rows = []
            for r, d, sl in runs(dur, cell):
                p = pick(d, cell, kind)
                rows.append((r, p))
            if mode == 'runs':
                for r, p in rows:
                    print(f'| {cell} | r{r} | {p["n"]} | ' + ' | '.join(fmt(p[k]) for k in KEYS) + ' |')
            else:
                med = {k: statistics.median(p[k] for _, p in rows) for k in KEYS}
                n = sum(p['n'] for _, p in rows)
                print(f'| {cell} | median of 3 | {n} | ' + ' | '.join(fmt(med[k]) for k in KEYS) + ' |')
