import sys, io, contextlib
sys.path.insert(0, '/tmp/claude-1000/-home-cdkbs-ckdbs--claude-worktrees-bp-per-core-residue/93896f60-b552-41c7-9a00-cc438c9ec661/scratchpad')
from lib import *

IDS = {
    'AB': ('cf889b63', '3e77e7d1'), 'AQ': ('cf889b63', '4cf30270'), 'S2S1': ('cf889b63', '1592007e'),
    'S4S6BQ': ('ccf9cfc1', '3e77e7d1'), 'MERGE': ('ccf9cfc1', '6139c2de'), 'S4': ('6139c2de', 'd555cb55'),
    'S6': ('d555cb55', '3e77e7d1'), 'MERGEALIGNED': ('ccf9cfc1al', '6139c2deal'),
    'PREMERGE_FIX': ('ccf9cfc1', 'rollfix'), 'CLOSE_FIX': ('3e77e7d1', 'rollfix'),
}
role = {'A': [], 'B': []}
for name, (ia, ib) in IDS.items():
    out, runs = load_pair(name)
    for key in (('update-range', 'literal'), ('update-range', 'prepared')):
        for side in 'AB':
            for x in out[key][side]:
                role[side].append((name, side, key[1], x['p99_us'] / 1000, x['mean_us'] / 1000, x['p50_us'] / 1000))
for side in 'AB':
    v = role[side]
    mx = [x[3] for x in v]
    print(side, 'arms', len(v), 'longest statement ms: min %.0f median %.0f max %.0f; arms > 300 ms: %d; > 500: %d' % (
        min(mx), statistics.median(mx), max(mx), sum(1 for m in mx if m > 300), sum(1 for m in mx if m > 500)))
# per pair
for name in IDS:
    out, runs = load_pair(name)
    row = []
    for side in 'AB':
        mx = [x['p99_us'] / 1000 for k in (('update-range', 'literal'), ('update-range', 'prepared')) for x in out[k][side]]
        row.append((round(min(mx)), round(max(mx))))
    print(name, 'A-role longest ms', row[0], 'B-role', row[1])
# other shapes: p99 by role
for shape in ('select-pk', 'update-lit'):
    for side in 'AB':
        v = []
        for name in IDS:
            out, runs = load_pair(name)
            for path in ('literal', 'prepared'):
                if (shape, path) in out:
                    v += [x['p99_us'] for x in out[(shape, path)][side]]
        print(shape, side, 'n', len(v), 'p99 us min %.0f median %.0f max %.0f' % (min(v), statistics.median(v), max(v)))
