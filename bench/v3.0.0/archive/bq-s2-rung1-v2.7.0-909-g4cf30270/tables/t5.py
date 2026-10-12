import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *
LC = C1 + ['c2', 'c3']
# slow share in opt3
W = [x for c in LC for r in (1, 2, 3) for x in inwin(load(S2, 'relaxed', c, 'opt3', r))]
tot = sum(x['hold_us'] for x in W)
slow = [x for x in W if x['hold_us'] > 150000]
print('opt3 rolls', len(W), 'slow>150ms', len(slow), 'share of summed hold', sum(x['hold_us'] for x in slow) / tot)
print('opt3 rolls >=50ms', sum(1 for x in W if x['hold_us'] >= 50000), 'share of summed hold', sum(x['hold_us'] for x in W if x['hold_us'] >= 50000) / tot)
# fast mode runs: runs with hold p50 < 50ms
for arm in ('base', 'opt3'):
    W = [x for c in LC for r in (1, 2, 3) for x in inwin(load(S2, 'relaxed', c, arm, r))]
    print(arm, 'p0', us(min(x['hold_us'] for x in W)), 'p1', us(nr([x['hold_us'] for x in W], 1)), 'p5', us(nr([x['hold_us'] for x in W], 5)))
# floors
print('arm | rolls/s | p0 hold | p0*rate share | measured median share | 15-run median share')
for arm in ['base (S1)'] + ARMS:
    js = [load(S1, 'relaxed', c, None, r) if arm == 'base (S1)' else load(S2, 'relaxed', c, arm, r) for c in LC for r in (1, 2, 3)]
    rate = st.mean(len(inwin(j)) / ((j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9) for j in js)
    p0 = min(x['hold_us'] for j in js for x in inwin(j))
    shares = [sum(x['hold_us'] for x in inwin(j)) / 1e3 / (j['m1_mono_ns'] - j['m0_mono_ns']) for j in js]
    print(arm, f'{rate:.3f}', us(p0), f'{100 * rate * p0 / 1e6:.2f} %', f'{100 * med(shares):.1f} %', f'[{100*min(shares):.1f}, {100*max(shares):.1f}]')
# first-roll holds vs later (young volume): for s256/s1024 rolls by seg number
for arm in ('s256', 's1024', 'base'):
    by = {}
    for c in LC:
        for r in (1, 2, 3):
            for x in inwin(load(S2, 'relaxed', c, arm, r)):
                by.setdefault(x['seg'] if arm != 'base' else min(x['seg'], 10), []).append(x['hold_us'])
    print(arm, {k: (len(v), us(med(v))) for k, v in sorted(by.items())[:12]})
