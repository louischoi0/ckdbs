import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

for cell, arm, r in (('c1-segregated-no0', 's1024', 1), ('c1-segregated-no0', 's1024', 2), ('c1-segregated-no0', 's1024', 3), ('c1-spread', 's1024', 3), ('c1-spread', 's1024', 1)):
    j = load(S2, 'relaxed', cell, arm, r)
    w = inwin(j)
    print(cell, arm, r, 'rolls', [(x['seg'], x['core'], us(x['hold_us'])) for x in w])
    print('  clients:', [(c['role'], int(c['core']), int(c['core_end']), c['ok']) for c in j['clients']])
    print('  moves', j['moves_in_window'], 'per-core agg p99:', {k: us(v['p99_us']) for k, v in j['read_by_core'].items()}, 'max', {k: us(v['max_us']) for k, v in j['read_by_core'].items()})
    for k in range(4):
        print('   core', k, 'stream wait', us(delta(j, f'load_c{k}_waited_wal_stream_us')), 'drain', us(delta(j, f'load_c{k}_wal_drain_latch_wait_us')), 'roll-blocked', us(delta(j, f'load_c{k}_wal_roll_blocked_wait_us')), 'heads', delta(j, f'load_c{k}_heads'))
# seg numbers of rolls in each arm
import collections
for arm in ARMS:
    segs = collections.Counter()
    for cell in CELLS + ['c3']:
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', cell, arm, r)
            for x in inwin(j):
                segs[x['seg']] += 1
    print(arm, 'seg numbers of in-window rolls: min', min(segs), 'max', max(segs))
