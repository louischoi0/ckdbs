import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

print('opt3/base per run: removed, first_segment, live at last roll, hold p50, share<50ms, rows/s, ckpt longest')
for arm in ('base', 'opt3'):
    for cell in CELLS + ['c3']:
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', cell, arm, r)
            w = inwin(j)
            H = [x['hold_us'] for x in w]
            live = [x['sync_segs'] for x in w]
            # per roll: slow (>150ms) vs live segs
            print(arm, cell, r, 'removed', delta(j, 'wal_segments_removed'), 'firstseg_m1', j['meta_m1'].get('wal_first_segment'),
                  'live', live[0] if live else None, '->', live[-1] if live else None,
                  'p50', us(nr(H, 50)), 'fast', sum(1 for h in H if h < 50000), '/', len(H),
                  'rows/s', round(j['inserter_rows_per_s']), 'ckpt_longest', us(j['meta_longest'].get('contention_checkpoint_longest_us')),
                  'ckpts', delta(j, 'checkpoint_runs'))
# correlation within opt3: hold vs live segs
import statistics as st
xs, ys = [], []
for cell in CELLS + ['c3']:
    for r in (1, 2, 3):
        j = load(S2, 'relaxed', cell, 'opt3', r)
        for x in inwin(j):
            xs.append(x['sync_segs']); ys.append(x['hold_us'])
def corr(a, b):
    ma, mb = st.mean(a), st.mean(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = (sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b)) ** .5
    return num / den
print('opt3 corr(hold, live segs)', corr(xs, ys), 'n', len(xs))
# slow vs fast rolls: live segs distribution
slow = [x for x, y in zip(xs, ys) if y > 150000]
fast = [x for x, y in zip(xs, ys) if y < 50000]
print('live segs: slow median', med(slow), 'n', len(slow), 'fast median', med(fast), 'n', len(fast))
# what is slow rolls' step
sl = []
for cell in CELLS + ['c3']:
    for r in (1, 2, 3):
        j = load(S2, 'relaxed', cell, 'opt3', r)
        sl += [x for x in inwin(j) if x['hold_us'] > 150000]
tot = sum(x['hold_us'] for x in sl)
for k in ('sync_gate_us', 'sync_us', 'header_us', 'dirsync_us', 'rename_us', 'seal_us', 'body_wait_us'):
    print('slow rolls', k, round(100 * sum(x[k] for x in sl) / tot, 1), '%')
print('n slow', len(sl))
fa = []
for cell in CELLS + ['c3']:
    for r in (1, 2, 3):
        j = load(S2, 'relaxed', cell, 'opt3', r)
        fa += [x for x in inwin(j) if x['hold_us'] < 50000]
tot = sum(x['hold_us'] for x in fa)
for k in ('sync_gate_us', 'sync_us', 'header_us', 'dirsync_us', 'rename_us', 'seal_us', 'body_wait_us'):
    print('fast rolls', k, round(100 * sum(x[k] for x in fa) / tot, 1), '%', 'median', us(nr([x[k] for x in fa], 50)))
print('n fast', len(fa))
