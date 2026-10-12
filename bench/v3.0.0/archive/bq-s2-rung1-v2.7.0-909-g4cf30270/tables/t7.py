import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *
LC = C1 + ['c2', 'c3']
# (i) rolls vs counter
bad = []
tot = 0
for arm in ARMS:
    for c in LC:
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', c, arm, r)
            a, b = len(inwin(j)), delta(j, 'wal_rolls')
            tot += 1
            if a != b:
                bad.append((arm, c, r, a, b))
print('runs', tot, 'rolls mismatch', bad)
bad = []
for c in LC:
    for r in (1, 2, 3):
        j = load(S1, 'relaxed', c, None, r)
        if len(inwin(j)) != delta(j, 'wal_rolls'):
            bad.append((c, r, len(inwin(j)), delta(j, 'wal_rolls')))
print('S1 mismatch', bad)
# (ii) moves
mv = [load(S2, 'relaxed', c, a, r)['moves_in_window'] for c in C1 for a in ARMS for r in (1, 2, 3)]
print('moves_in_window C1 range', min(mv), max(mv), 'median', med(mv))
mv2 = [load(S2, 'relaxed', 'c2', a, r)['moves_in_window'] for a in ARMS for r in (1, 2, 3)]
print('moves C2', min(mv2), max(mv2))
# (iii)
j = load(S2, 'relaxed', 'c2', 's1024', 1)
print('c2 s1024 r1 holds', [(x['seg'], us(x['hold_us']), x['body']) for x in inwin(j)])
W = [x for c in LC for r in (1, 2, 3) for x in inwin(load(S2, 'relaxed', c, 's256', r))]
hit = [x for x in W if x['body'] == 'hit']
print('s256 hits', len(hit), 'median hold', us(med([x['hold_us'] for x in hit])), 'median gate', us(med([x['sync_gate_us'] for x in hit])))
miss = [x for x in W if x['body'] != 'hit']
print('s256 miss', len(miss), 'median hold', us(med([x['hold_us'] for x in miss])))
W = [x for c in LC for r in (1, 2, 3) for x in inwin(load(S2, 'relaxed', c, 's1024', r))]
hit = [x for x in W if x['body'] == 'hit']
print('s1024 hits', len(hit), [us(x['hold_us']) for x in hit])
# (iv)
j = load(S2, 'relaxed', 'c1-segregated-no0', 's1024', 1)
print('span', (j['m1_mono_ns'] - j['m0_mono_ns']) / 1e9, 'measure window', (j['stop_at_mono_ns'] - j['measure_from_mono_ns']) / 1e9)
for arm in ARMS:
    sp = [(load(S2, 'relaxed', c, arm, r)['m1_mono_ns'] - load(S2, 'relaxed', c, arm, r)['m0_mono_ns']) / 1e9 for c in LC for r in (1, 2, 3)]
    print(arm, 'm1-m0 span: min', round(min(sp), 1), 'median', round(med(sp), 1), 'max', round(max(sp), 1))
# (vii)
dr = [(c, a, r, sum(cl['slow_dropped'] for cl in load(S2, 'relaxed', c, a, r)['clients'] if cl['role'] in ('reader', 'writer'))) for c in CELLS for a in ARMS for r in (1, 2, 3)]
print('reader/writer slow_dropped nonzero', [x for x in dr if x[3]])
dw = max(sum(cl['slow_dropped'] for cl in load(S2, 'relaxed', c, a, r)['clients'] if cl['role'] == 'walker') for c in C1 for a in ARMS for r in (1, 2, 3))
print('walker slow_dropped max', dw)
