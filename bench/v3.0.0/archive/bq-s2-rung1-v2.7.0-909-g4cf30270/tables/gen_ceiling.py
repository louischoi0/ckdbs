import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from lib import *

OUT = []


def P(s=''):
    OUT.append(s)


def pct(slow_us, n, q):
    """nearest rank over n samples of which the slow ones (sorted ascending) are given; the rest are < 1 ms."""
    k = max(1, math.ceil(Fraction(str(q)) * n / 100))
    fast = n - len(slow_us)
    return slow_us[k - fast - 1] if k > fast else 0.0


P('**Ceiling with every roll-aligned stall removed (C1, relaxed): the readers\' schedule p99 and p99.9 after dropping each sample whose wait [due, due + latency] overlaps any in-window roll hold**')
P()
P('This is a bound computed from the recorded slow samples (those ≥ 1 ms), not a run: it removes the stalls a roll overlaps and leaves every other cause in. '
  '`< 1 ms` means the percentile falls inside the samples the driver does not keep.')
P()
P('| cell | arm | run | readers\' samples | samples ≥ 1 ms | of them overlapping a roll | p99 as measured | p99 with roll-overlapping samples removed | p99.9 as measured | p99.9 with them removed | p99 removed ÷ largest isolated p99 |')
P('|---|---|---|---|---|---|---|---|---|---|---|')
rows = []
for cell in C1:
    s1 = [load(S1, 'relaxed', ISO[cell], None, r)['read_from_schedule']['p99_us'] for r in (1, 2, 3)]
    s2 = load(S2, 'relaxed', ISO[cell], 'base', 1)['read_from_schedule']['p99_us']
    isomax = max(s1 + [s2])
    for arm in ARMS:
        for r in (1, 2, 3):
            j = load(S2, 'relaxed', cell, arm, r)
            w = inwin(j)
            ivs = [(x['start_ns'], x['end_ns']) for x in w]
            cs = [c for c in j['clients'] if c['role'] == 'reader']
            n = sum(c['ok'] + c['errors'] + c['refusals'] for c in cs)
            slow, kept = [], []
            for c in cs:
                for t, ns in c['slow']:
                    slow.append(ns / 1000)
                    if not any(t <= b and t + ns >= a for a, b in ivs):
                        kept.append(ns / 1000)
            slow.sort(); kept.sort()
            removed = len(slow) - len(kept)
            nn = n - removed
            p99, p999 = pct(slow, n, 99), pct(slow, n, 99.9)
            q99, q999 = pct(kept, nn, 99), pct(kept, nn, 99.9)
            f = lambda x: '< 1 ms' if x == 0.0 else us(x)
            rows.append((cell, arm, r, q99 / isomax))
            P(f'| {cell} | {arm} | r{r} | {n:,} | {len(slow):,} | {removed:,} ({100*removed/max(len(slow),1):.0f} %) | {f(p99)} | {f(q99)} | {f(p999)} | {f(q999)} | '
              f'{("< %.1fx" % (1000 / isomax)) if q99 == 0.0 else "%.1fx" % (q99 / isomax)} |')
P()
print('\n'.join(OUT))
