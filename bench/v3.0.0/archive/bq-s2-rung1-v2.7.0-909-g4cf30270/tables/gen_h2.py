import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

OUT = []


def P(s=''):
    OUT.append(s)


LOADCELLS = C1 + ['c2', 'c3']
P('**H2\'s prediction, clause by clause: what scales with the segment size (every relaxed load cell, 15 runs per arm)**')
P()
P('| arm | log rate (mean of 15 runs) | predicted rolls/s = rate ÷ size | measured rolls/s | hold p50 | held time per 30 s window (median [min, max]) | writer syncs/s (mean) | log per writer sync | rolls whose body was not ready |')
P('|---|---|---|---|---|---|---|---|---|')
for arm in ['base (BQ-S1)'] + ARMS:
    js = []
    for c in LOADCELLS:
        for r in (1, 2, 3):
            js.append(load(S1, 'relaxed', c, None, r) if arm.startswith('base (BQ') else load(S2, 'relaxed', c, arm, r))
    size = 64 if arm.startswith('base (BQ') else SEG[arm]
    rate = [delta(j, 'wal_append_lsn') / 1048576 / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) for j in js]
    meas = [len(inwin(j)) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) for j in js]
    held = [sum(x['hold_us'] for x in inwin(j)) / 1e6 / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) * 30 for j in js]
    H = [x['hold_us'] for j in js for x in inwin(j)]
    syn = [delta(j, 'wal_syncs') / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) for j in js]
    mps = [delta(j, 'wal_append_lsn') / 1048576 / max(delta(j, 'wal_syncs'), 1) for j in js]
    W = [x for j in js for x in inwin(j)]
    miss = sum(1 for x in W if x['body'] != 'hit')
    P(f'| {arm} ({size:,} MiB) | {st.mean(rate):.1f} MiB/s | {st.mean(rate)/size:.3f} /s | {st.mean(meas):.3f} /s | {us(nr(H,50))} | {med(held):.1f} s [{min(held):.1f}, {max(held):.1f}] | '
      f'{st.mean(syn):.1f} /s | {st.mean(mps):.2f} MiB | {miss} of {len(W)} |')
P()
print('\n'.join(OUT))
