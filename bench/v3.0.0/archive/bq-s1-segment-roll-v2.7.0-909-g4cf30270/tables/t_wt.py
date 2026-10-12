import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

LOADCELLS = ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread']
print('| durability | quantity (per roll) | rolls | p0 | p25 | p50 | p95 | p99 | max |')
print('|---|---|---|---|---|---|---|---|---|')
for dur in DURS:
    rs = [x for c in LOADCELLS for _, d, sl in runs(dur, c) for x in rolls(sl, d)[0]]
    for name, f in (('hold', lambda x: x['hold_us']),
                    ('sync gate wait (step c1)', lambda x: x['sync_gate_us']),
                    ('writer-sync tail fdatasync waited out', lambda x: x['waited_sync_tail_us']),
                    ('writer-sync non-tail fdatasyncs waited out', lambda x: x['waited_sync_rest_us']),
                    ('writer syncs waited out (count, not µs)', None),
                    ('live segments held at the roll\'s sync (count)', None)):
        if f is None:
            key = 'waited_syncs' if name.startswith('writer') else 'sync_segs'
            v = [x[key] for x in rs]
            print(f'| {dur} | {name} | {len(v)} | ' + ' | '.join(str(nr(v, q)) for q in (0, 25, 50, 95, 99)) + f' | {max(v)} |')
        else:
            v = [f(x) for x in rs]
            print(f'| {dur} | {name} | {len(v)} | ' + ' | '.join(fmt(nr(v, q)) for q in (0, 25, 50, 95, 99)) + f' | {fmt(max(v))} |')
