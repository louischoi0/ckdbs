import sys
sys.path.insert(0, '/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad')
from lib import *

IDS = {
    'AB': ('cf889b63', '3e77e7d1'), 'AQ': ('cf889b63', '4cf30270'), 'S2S1': ('cf889b63', '1592007e'),
    'S4S6BQ': ('ccf9cfc1', '3e77e7d1'), 'MERGE': ('ccf9cfc1', '6139c2de'), 'S4': ('6139c2de', 'd555cb55'),
    'S6': ('d555cb55', '3e77e7d1'), 'MERGEALIGNED': ('ccf9cfc1al', '6139c2deal'),
    'PREMERGE_FIX': ('ccf9cfc1', 'rollfix'), 'CLOSE_FIX': ('3e77e7d1', 'rollfix'),
}
pool = {}
for name, (ia, ib) in IDS.items():
    out, runs = load_pair(name)
    for key, arms in out.items():
        for i, (xa, xb) in enumerate(zip(arms['A'], arms['B'])):
            pool.setdefault((key, ia), []).append((name, i + 1, xa['server_cpu_us']))
            pool.setdefault((key, ib), []).append((name, i + 1, xb['server_cpu_us']))
order = ['cf889b63', '1592007e', 'ccf9cfc1', '4cf30270', '6139c2de', 'd555cb55', '3e77e7d1', 'rollfix', 'ccf9cfc1al', '6139c2deal']
for key in [('update-range', 'literal'), ('update-range', 'prepared'), ('select-pk', 'literal'), ('select-pk', 'prepared')]:
    print('---', key)
    for b in order:
        v = [x[2] for x in pool.get((key, b), [])]
        if v:
            print(f'{b:12} n={len(v):2} mean {sum(v)/len(v):8.2f} min {min(v):8.2f} max {max(v):8.2f} range% {100*(max(v)-min(v))/(sum(v)/len(v)):4.1f}  sd {statistics.stdev(v) if len(v)>1 else 0:6.2f} sessions {sorted(set(x[0] for x in pool[(key,b)]))}')
