import sys, statistics
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

KINDS = ['wal_stream', 'wal_sync_gate', 'frame_table', 'window', 'lock_partition', 'free_map']
print('| durability | cell | wal stream latch | WAL sync gate | frame table | window latch | lock partition | free map | roll-covered share of the stream-latch wait |')
print('|---|---|---|---|---|---|---|---|---|')
for dur in DURS:
    for cell in ['c3', 'c2', 'c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2-iso', 'c1-iso-segregated-no0']:
        vals = {k: [] for k in KINDS}
        cov = []
        for r, d, sl in runs(dur, cell):
            for k in KINDS:
                vals[k].append(delta(d, f'contention_{k}_wait_us'))
            st = delta(d, 'contention_wal_stream_wait_us')
            cov.append(delta(d, 'contention_wal_roll_blocked_wait_us') / st if st else None)
        cs = [c for c in cov if c is not None]
        print(f'| {dur} | {cell} | ' + ' | '.join(fmt(statistics.mean(vals[k])) for k in KINDS) + ' | ' + (f'{100 * statistics.mean(cs):.0f} %' if cs else 'n/a (no stream wait)') + ' |')
